/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "config.h"
#include "header.h"
#include "npb.h"
#include "npb_bookmark_format.h"
#include "power.h"
#include "reader.h"
#include "reader_menu.h"
#include "rtc.h"
#include "sd.h"
#include "text.h"

#define APP_READER_MAX_ROW_CHARS 64

#define APP_READER_BMK_DIR ".nanopage"

static struct npb_book book;
static char full_path_buf[APP_SD_MAX_PATH_LEN];
static char session_path_buf[APP_SD_MAX_PATH_LEN];
static char bmk_dir_buf[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 4];
static char bmk_path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + APP_SD_MAX_NAME_LEN + 8];
static void (*exit_cb)(void);
static bool book_open;

static enum app_text_font reader_font = APP_TEXT_FONT_TAMZEN;

static uint16_t content_top_px;
static uint16_t content_rows;
static uint16_t reader_font_height;

static uint32_t page_start_row;

static struct npb_bookmark_file bmk_state;

static bool page_bookmarked;
static uint32_t last_blob_offset;
static uint8_t last_font_id;

static uint8_t partial_count;

static bool sd_mount_held;

static void sd_idle_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (sd_mount_held) {
		app_sd_deinit();
		sd_mount_held = false;
	}
}

static K_WORK_DELAYABLE_DEFINE(sd_idle_work, sd_idle_work_handler);

/* Keeps the SD card mounted/powered for a short grace window after use, so a
 * burst of quick actions (rapid page turns, TOC/bookmark browsing) shares one
 * mount instead of paying app_sd_init()'s mount+power-up cost on every one.
 * The card draws real current while merely powered-but-idle on this board, so
 * the grace window must stay short and always resolve to a hard teardown
 * (sd_force_release()) before entering any low-power state - see
 * sd_force_release()'s callers.
 */
static int sd_ensure_active(void)
{
	int ret;

	if (!sd_mount_held) {
		ret = app_sd_init();
		if (ret) {
			return ret;
		}
		sd_mount_held = true;
	}

	k_work_reschedule(&sd_idle_work, K_MSEC(APP_READER_SD_IDLE_MS));

	return 0;
}

/* Cancels any pending grace-window teardown and unmounts/powers off the SD
 * card immediately. Used wherever the reader must guarantee the card is off
 * before returning - closing the reader, giving up on an unrecoverable font
 * switch, and flushing state before the board sleeps.
 */
static void sd_force_release(void)
{
	k_work_cancel_delayable(&sd_idle_work);

	if (sd_mount_held) {
		app_sd_deinit();
		sd_mount_held = false;
	}
}

static int bookmark_index_for_offset(uint32_t offset)
{
	for (uint8_t i = 0; i < bmk_state.bookmark_count; i++) {
		if (bmk_state.bookmarks[i].text_offset == offset) {
			return i;
		}
	}

	return -1;
}

static uint32_t current_blob_offset(void);

static void reader_flush(void)
{
	partial_count++;

	if (partial_count >= APP_READER_FULL_REFRESH_EVERY) {
		app_text_full_refresh();
		partial_count = 0;
	} else {
		app_text_flush();
	}
}

static void render_page(void)
{
	char buf[APP_READER_MAX_ROW_CHARS];

	if (npb_open(full_path_buf, reader_font, &book)) {
		return;
	}

	for (uint16_t i = 0; i < content_rows; i++) {
		uint16_t y_px = content_top_px + i * reader_font_height;
		uint32_t row_index = page_start_row + i;
		struct npb_row row;
		const char *text = "";

		if (row_index < book.row_count && npb_read_row(&book, row_index, &row) == 0 &&
		    row.length != 0) {
			size_t len = MIN(row.length, sizeof(buf) - 1);

			if (npb_read_text(&book, row.offset, (uint8_t)len, buf) == 0) {
				buf[len] = '\0';
				text = buf;
			}
		}

		app_text_draw_text_px(y_px, text, reader_font);
	}

	last_blob_offset = current_blob_offset();
	last_font_id = book.font_id;
	page_bookmarked = bookmark_index_for_offset(last_blob_offset) >= 0;

	npb_close(&book);
}

static uint32_t current_blob_offset(void)
{
	struct npb_row row;

	if (page_start_row < book.row_count && npb_read_row(&book, page_start_row, &row) == 0) {
		return row.offset;
	}

	return 0;
}

static void save_bookmark(void)
{
	struct fs_file_t f;
	uint32_t minutes;
	int ret;

	memcpy(bmk_state.magic, NPB_BOOKMARK_MAGIC, sizeof(bmk_state.magic));
	bmk_state.version = NPB_BOOKMARK_VERSION;
	bmk_state.last_font_id = last_font_id;
	bmk_state.last_position = last_blob_offset;
	if (app_rtc_get_minutes_since_2000(&minutes) == 0) {
		bmk_state.last_read_local = minutes;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, bmk_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		printk("reader: failed to open bookmark sidecar (%d) - position not saved\n", ret);
		return;
	}

	ret = fs_write(&f, &bmk_state, sizeof(bmk_state));
	if (ret < 0 || (size_t)ret != sizeof(bmk_state)) {
		printk("reader: failed to write bookmark sidecar (%d)\n", ret);
	}

	fs_close(&f);
}

#define APP_READER_SESSION_DIR_REL "/" APP_READER_BMK_DIR
#define APP_READER_SESSION_FILE_NAME "session"

static int session_marker_path(char *out, size_t out_size, bool create_dir)
{
	char dir[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 4];
	int ret;

	ret = app_sd_full_path(APP_READER_SESSION_DIR_REL, dir, sizeof(dir));
	if (ret) {
		return ret;
	}

	if (create_dir) {
		ret = fs_mkdir(dir);
		if (ret && ret != -EEXIST) {
			return ret;
		}
	}

	ret = snprintf(out, out_size, "%s/" APP_READER_SESSION_FILE_NAME, dir);
	return (ret < 0 || (size_t)ret >= out_size) ? -ENAMETOOLONG : 0;
}

static void clear_session_marker(void)
{
	char path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 24];

	if (session_marker_path(path, sizeof(path), false) == 0) {
		fs_unlink(path);
	}
}

static void flush_pending_state(void)
{
	char path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 24];

	k_work_cancel_delayable(&sd_idle_work);

	if (!sd_mount_held) {
		if (app_sd_init()) {
			printk("reader: SD unavailable, pending state not flushed before sleep\n");
			return;
		}
		sd_mount_held = true;
	}

	if (book_open) {
		save_bookmark();

		if (session_marker_path(path, sizeof(path), true) == 0) {
			struct fs_file_t f;

			fs_file_t_init(&f);
			if (fs_open(&f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) == 0) {
				fs_write(&f, session_path_buf, strlen(session_path_buf));
				fs_close(&f);
			}
		}
	} else {
		clear_session_marker();
	}

	app_sd_deinit();
	sd_mount_held = false;
}

static void load_bookmark(void)
{
	struct npb_bookmark_file on_disk;
	struct fs_file_t f;
	int ret;

	page_start_row = 0;

	memset(&bmk_state, 0, sizeof(bmk_state));
	memcpy(bmk_state.magic, NPB_BOOKMARK_MAGIC, sizeof(bmk_state.magic));
	bmk_state.version = NPB_BOOKMARK_VERSION;

	fs_file_t_init(&f);
	ret = fs_open(&f, bmk_path, FS_O_READ);
	if (ret) {
		return;
	}

	memset(&on_disk, 0, sizeof(on_disk));
	ret = fs_read(&f, &on_disk, sizeof(on_disk));
	fs_close(&f);

	if ((ret != NPB_BOOKMARK_FILE_SIZE_V1 && ret != sizeof(on_disk)) ||
	    memcmp(on_disk.magic, NPB_BOOKMARK_MAGIC, sizeof(on_disk.magic)) != 0) {
		return;
	}

	bmk_state = on_disk;
	if (bmk_state.bookmark_count > NPB_MAX_BOOKMARKS) {
		bmk_state.bookmark_count = NPB_MAX_BOOKMARKS;
	}

	uint32_t row_index;

	if (npb_find_row(&book, bmk_state.last_position, &row_index) == 0) {
		page_start_row = row_index;
	}
}

static void toggle_bookmark(void)
{
	uint32_t offset;
	int idx;

	if (!book_open) {
		return;
	}

	if (sd_ensure_active()) {
		printk("reader: SD unavailable, bookmark not toggled\n");
		return;
	}

	if (npb_open(full_path_buf, reader_font, &book) != 0) {
		return;
	}
	offset = current_blob_offset();
	last_font_id = book.font_id;
	npb_close(&book);
	last_blob_offset = offset;

	idx = bookmark_index_for_offset(offset);

	if (idx >= 0) {
		memmove(&bmk_state.bookmarks[idx], &bmk_state.bookmarks[idx + 1],
			(size_t)(bmk_state.bookmark_count - idx - 1) *
				sizeof(bmk_state.bookmarks[0]));
		bmk_state.bookmark_count--;
		page_bookmarked = false;
		printk("reader: bookmark removed at offset %u\n", offset);
	} else if (bmk_state.bookmark_count < NPB_MAX_BOOKMARKS) {
		struct npb_bookmark *slot = &bmk_state.bookmarks[bmk_state.bookmark_count++];

		slot->text_offset = offset;
		memset(slot->label, 0, sizeof(slot->label));
		slot->reserved = 0;
		page_bookmarked = true;
		printk("reader: bookmark added at offset %u\n", offset);
	} else {
		printk("reader: bookmark list full (%d), not adding\n", NPB_MAX_BOOKMARKS);
		return;
	}

	save_bookmark();

	if (app_header_update("", page_bookmarked) > 0) {
		app_text_flush();
	}
}

static void turn_page(int direction)
{
	if (direction < 0) {
		if (page_start_row == 0) {
			return;
		}
		page_start_row = (page_start_row > content_rows) ? page_start_row - content_rows
								   : 0;
	} else {
		if (page_start_row + content_rows >= book.row_count) {
			return;
		}
		page_start_row += content_rows;
	}

	if (sd_ensure_active()) {
		printk("reader: SD unavailable, page turn not saved\n");
		return;
	}

	render_page();

	app_header_update("", page_bookmarked);

	reader_flush();

	save_bookmark();
}

/* Disables all reader input/power hooks and marks the reader closed. Shared
 * by every path that gives up on the current reading session - close_reader()
 * and app_reader_set_font()'s revert-failed fallback - so they unwind with
 * the same shape instead of each hand-rolling their own subset of resets.
 */
static void teardown_reader_hooks(void)
{
	book_open = false;

	app_buttons_set_press_callback(NULL);
	app_buttons_set_long_press_callback(NULL);
	app_buttons_set_combo_callback(NULL);
	app_power_set_idle_hook(NULL);
}

static void close_reader(void)
{
	void (*cb)(void) = exit_cb;

	/* Synchronize with the idle-timeout System OFF path before touching
	 * any of the state it also reads/writes via flush_pending_state()
	 * (book_open, bmk_state, exit_cb, ...). If a System OFF entry is
	 * already in flight, this blocks until it finishes - which, since it
	 * ends in sys_poweroff(), means this thread never resumes, which is
	 * fine: the state was already flushed and the board is powering off.
	 */
	app_power_cancel_idle_sync();

	if (sd_ensure_active() == 0) {
		save_bookmark();
		clear_session_marker();
	}
	sd_force_release();

	exit_cb = NULL;
	teardown_reader_hooks();

	if (cb) {
		cb();
	}
}

static void on_button(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		turn_page(-1);
		break;
	case APP_BUTTON_NEXT:
		turn_page(1);
		break;
	case APP_BUTTON_ACTION:
		close_reader();
		break;
	}
}

static void open_reader_menu(void);

static void reopen_reading(void)
{
	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);
	app_buttons_set_combo_callback(toggle_bookmark);

	if (sd_ensure_active()) {
		printk("reader: SD unavailable, can't redraw after menu/toc\n");
		return;
	}

	render_page();
	app_header_update("", page_bookmarked);
	app_text_full_refresh();
	partial_count = 0;
}

static void open_reader_menu(void)
{
	app_buttons_set_combo_callback(NULL);
	app_reader_menu_show(reopen_reading);
}

static int open_book_at_font(uint32_t resume_offset, bool have_resume_offset)
{
	uint16_t grid_cols, grid_rows, chrome_row_height, reader_glyph_width, reader_cols;
	uint16_t content_bottom_px;
	int ret;

	/* Grid/font metrics don't touch `book`, so they're validated up front -
	 * that keeps this function to the same single-open/single-close shape
	 * as render_page()/toggle_bookmark()/app_reader_jump_to_offset(), with
	 * no npb_close() duplicated across early-error returns.
	 */
	app_text_get_grid(&grid_cols, &grid_rows);
	if (grid_rows < 3) {
		return -ENOSPC;
	}
	chrome_row_height = app_text_row_height();
	content_top_px = chrome_row_height;
	content_bottom_px = (uint16_t)(grid_rows - 1) * chrome_row_height;

	app_text_font_info(reader_font, &reader_glyph_width, &reader_font_height, &reader_cols);
	content_rows = (content_bottom_px - content_top_px) / reader_font_height;
	if (content_rows == 0) {
		return -ENOSPC;
	}

	ret = npb_open(full_path_buf, reader_font, &book);
	if (ret) {
		printk("reader: failed to open \"%s\" at font %d (%d)\n", full_path_buf,
		       reader_font, ret);
		return ret;
	}

	if (have_resume_offset) {
		uint32_t row_index;

		page_start_row = (npb_find_row(&book, resume_offset, &row_index) == 0)
					 ? row_index
					 : 0;
	} else {
		load_bookmark();
	}

	npb_close(&book);

	return 0;
}

static int build_bmk_paths(const char *path)
{
	char rel_dir[APP_SD_MAX_PATH_LEN];
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	size_t dir_len = slash ? (size_t)(slash - path) : 0;
	int ret;

	if (dir_len + sizeof("/" APP_READER_BMK_DIR) > sizeof(rel_dir)) {
		return -ENAMETOOLONG;
	}
	memcpy(rel_dir, path, dir_len);
	strcpy(rel_dir + dir_len, "/" APP_READER_BMK_DIR);

	ret = app_sd_full_path(rel_dir, bmk_dir_buf, sizeof(bmk_dir_buf));
	if (ret) {
		return ret;
	}

	ret = snprintf(bmk_path, sizeof(bmk_path), "%s/%s.bmk", bmk_dir_buf, base);
	if (ret < 0 || (size_t)ret >= sizeof(bmk_path)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

int app_reader_open(const char *path, void (*on_exit)(void))
{
	int ret;

	strncpy(session_path_buf, path, sizeof(session_path_buf) - 1);
	session_path_buf[sizeof(session_path_buf) - 1] = '\0';

	ret = sd_ensure_active();
	if (ret) {
		printk("reader: SD init failed (%d)\n", ret);
		return ret;
	}

	ret = app_sd_full_path(path, full_path_buf, sizeof(full_path_buf));
	if (ret) {
		printk("reader: couldn't resolve \"%s\" (%d)\n", path, ret);
		sd_force_release();
		return ret;
	}

	ret = build_bmk_paths(path);
	if (ret) {
		printk("reader: bookmark sidecar path too long for \"%s\"\n", path);
		sd_force_release();
		return ret;
	}

	ret = fs_mkdir(bmk_dir_buf);
	if (ret && ret != -EEXIST) {
		printk("reader: couldn't create bookmark sidecar folder \"%s\" (%d)\n", bmk_dir_buf,
		       ret);
	}

	ret = open_book_at_font(0, false);
	if (ret) {
		sd_force_release();
		return ret;
	}

	exit_cb = on_exit;
	partial_count = 0;
	book_open = true;

	render_page();
	app_header_update("", page_bookmarked);

	ret = app_text_full_refresh();
	if (ret) {
		exit_cb = NULL;
		book_open = false;
		sd_force_release();
		return ret;
	}

	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);
	app_buttons_set_combo_callback(toggle_bookmark);
	app_power_set_idle_hook(flush_pending_state);

	return 0;
}

int app_reader_resume_last_session(void (*on_exit)(void))
{
	char path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 24];
	char saved_path[APP_SD_MAX_PATH_LEN];
	struct fs_file_t f;
	int ret;
	int n;

	ret = app_sd_init();
	if (ret) {
		return ret;
	}

	ret = session_marker_path(path, sizeof(path), false);
	if (ret) {
		app_sd_deinit();
		return ret;
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret) {
		app_sd_deinit();
		return ret;
	}

	n = fs_read(&f, saved_path, sizeof(saved_path) - 1);
	fs_close(&f);
	app_sd_deinit();

	if (n <= 0) {
		return -ENOENT;
	}
	saved_path[n] = '\0';

	printk("reader: resuming last session at \"%s\"\n", saved_path);

	return app_reader_open(saved_path, on_exit);
}

enum app_text_font app_reader_get_font(void)
{
	return reader_font;
}

const char *app_reader_get_path(void)
{
	return full_path_buf;
}

int app_reader_jump_to_offset(uint32_t text_offset)
{
	struct npb_row row;
	uint32_t row_index;
	int ret;

	if (!book_open) {
		return -EINVAL;
	}

	ret = sd_ensure_active();
	if (ret) {
		return ret;
	}

	ret = npb_open(full_path_buf, reader_font, &book);
	if (ret) {
		return ret;
	}

	ret = npb_find_row(&book, text_offset, &row_index);
	if (ret == 0) {
		ret = npb_read_row(&book, row_index, &row);
	}
	last_font_id = book.font_id;
	npb_close(&book);

	if (ret) {
		return ret;
	}

	page_start_row = row_index;
	last_blob_offset = row.offset;
	save_bookmark();

	return 0;
}

void app_reader_set_font(enum app_text_font font)
{
	if (font == reader_font) {
		return;
	}

	if (!book_open) {
		reader_font = font;
		return;
	}

	enum app_text_font old_font = reader_font;
	uint32_t resume_offset = 0;

	if (sd_ensure_active()) {
		printk("reader: SD init failed for font change\n");
		return;
	}

	if (npb_open(full_path_buf, old_font, &book) == 0) {
		resume_offset = current_blob_offset();
		npb_close(&book);
	}

	reader_font = font;

	if (open_book_at_font(resume_offset, true)) {
		printk("reader: font %d unavailable for this book, reverting to %d\n", font,
		       old_font);
		reader_font = old_font;
		if (open_book_at_font(resume_offset, true)) {
			teardown_reader_hooks();
			clear_session_marker();
			sd_force_release();
			return;
		}
	}

	render_page();
	app_header_update("", page_bookmarked);
	app_text_full_refresh();
	partial_count = 0;
}

bool app_reader_is_active(void)
{
	return book_open;
}

bool app_reader_current_page_bookmarked(void)
{
	return book_open && page_bookmarked;
}

uint16_t app_reader_get_content_rows(void)
{
	return book_open ? content_rows : 0;
}

int app_reader_get_bookmarks(struct npb_bookmark *out, size_t max, size_t *count)
{
	size_t n;

	if (!book_open) {
		return -EINVAL;
	}

	n = MIN((size_t)bmk_state.bookmark_count, max);

	memcpy(out, bmk_state.bookmarks, n * sizeof(out[0]));
	*count = n;

	return 0;
}
