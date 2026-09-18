/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "config.h"
#include "npb.h"
#include "npb_bookmark_format.h"
#include "reader.h"
#include "reader_menu.h"
#include "sd.h"
#include "text.h"

/* APP_READER_FULL_REFRESH_EVERY (config.h): see browser.h's
 * APP_BROWSER_FULL_REFRESH_EVERY for the identical reasoning - scoped
 * separately rather than shared, same as header.c/footer.c/browser.c each
 * owning their own refresh decisions.
 */

/* Longest single row this firmware will ever render - a struct npb_row's
 * length is a uint8_t (max 255), but every font variant this app supports
 * wraps well under that (49 cols at the smallest/widest-grid font); sized
 * generously above that rather than tied to it, since a plain stack buffer
 * here costs nothing.
 */
#define APP_READER_MAX_ROW_CHARS 64

static struct npb_book book;
static char full_path_buf[APP_SD_MAX_PATH_LEN];
static char bmk_path[APP_SD_MAX_PATH_LEN + 8]; /* "<path>" + ".bmk" + NUL */
static void (*exit_cb)(void);
static bool book_open;

/*
 * Font book content renders in - defaults to the smallest
 * (APP_TEXT_FONT_TAMZEN, matching the rest of the UI) rather than a fixed
 * #define, specifically so app_reader_set_font() below can change it - a
 * future settings menu's hook, not wired to any UI yet (no button is free
 * for it: Prev/Next/Action are already paging/closing - see reader.h).
 */
static enum app_text_font reader_font = APP_TEXT_FONT_TAMZEN;

/* Pixel row where the content area starts (just below the header) and how
 * many APP_READER_FONT-height lines fit before the footer - computed once
 * in app_reader_open() from whatever the active (small) font's header/
 * footer row height actually is, so this keeps working if that ever
 * changes. See APP_READER_FONT's own doc comment for why content uses a
 * different, pixel-addressed font/height than the header/footer's row grid.
 */
static uint16_t content_top_px;
static uint16_t content_rows;
static uint16_t reader_font_height;

/* Row-table index of the first row currently shown on screen. */
static uint32_t page_start_row;

static uint8_t partial_count;

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

/*
 * Book content renders in `reader_font` (see its own doc comment above),
 * independent of whatever font header.c/footer.c/browser.c are using for
 * the rest of the UI. app_text_draw_text_px() (text.h) draws with it,
 * addressed by absolute pixel row rather than a row index into the active
 * font's grid - required since a font whose height differs from the
 * active font's row height (e.g. APP_TEXT_FONT_TAMZEN_LARGE's 24px lines
 * against the header/footer's 16px rows) can't be addressed by row index
 * at all - see that function's own doc comment. Tamzen's native ink fills
 * the whole cell for both MEDIUM and LARGE (no bottom-padding slack to
 * redistribute the way the smallest font has), so there's no equivalent to
 * the small font's bottom-aligned "free gap below the header" trick for
 * those two - every line, including the first, draws through the same
 * top-aligned path regardless of which of the three is active.
 *
 * Re-opens `book`'s file handle for the duration of this call and closes
 * it again before returning - the SD card's own power/mount is a much
 * coarser, caller-held bracket (turn_page()/app_reader_open()/
 * app_reader_set_font() each hold one app_sd_init()/app_sd_deinit() pair
 * around their whole action), but the underlying fs_file_t is cheap
 * enough to reopen (a header read + a small variant-table scan) that
 * there's no reason to also keep it resident across page turns - doing so
 * would mean carrying a stale handle across an app_sd_deinit() unmount,
 * which is what this file used to do back when the card stayed mounted
 * for the browser/reader's entire lifetime. `book`'s other fields (row
 * count, table offsets, font_id) are left untouched by npb_close(), so
 * they stay valid and reusable (e.g. turn_page()'s bounds check) even
 * while the file itself is closed between calls.
 */
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

	npb_close(&book);
}

/* The blob offset of whatever's currently at the top of the visible page -
 * what gets persisted as the sidecar's resume position. Falls back to 0
 * (start of book) if the row read fails or the page is past the end.
 * Assumes `book`'s file is already open (see render_page()'s doc comment
 * on why the file itself is opened/closed per call rather than held
 * across the reading session) - callers reopen it around this the same
 * way render_page() does.
 */
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
	struct npb_bookmark_file bmk;
	struct fs_file_t f;
	uint32_t last_position = 0;
	int ret;

	if (npb_open(full_path_buf, reader_font, &book) == 0) {
		last_position = current_blob_offset();
		npb_close(&book);
	}

	memset(&bmk, 0, sizeof(bmk));
	memcpy(bmk.magic, NPB_BOOKMARK_MAGIC, sizeof(bmk.magic));
	bmk.version = NPB_BOOKMARK_VERSION;
	bmk.last_font_id = book.font_id;
	bmk.bookmark_count = 0;
	bmk.last_position = last_position;

	fs_file_t_init(&f);
	ret = fs_open(&f, bmk_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		printk("reader: failed to open bookmark sidecar (%d) - position not saved\n", ret);
		return;
	}

	ret = fs_write(&f, &bmk, sizeof(bmk));
	if (ret < 0 || (size_t)ret != sizeof(bmk)) {
		printk("reader: failed to write bookmark sidecar (%d)\n", ret);
	}

	fs_close(&f);
}

/* Loads the sidecar (if any) and resolves its last_position to a starting
 * row via npb_find_row() - book-format.md's binary-search primitive.
 * Leaves page_start_row at 0 (start of book) if there's no sidecar yet, or
 * if anything about reading it fails - never treated as a fatal error, an
 * unreadable/missing sidecar just means "nothing to resume."
 */
static void load_bookmark(void)
{
	struct npb_bookmark_file bmk;
	struct fs_file_t f;
	int ret;

	page_start_row = 0;

	fs_file_t_init(&f);
	ret = fs_open(&f, bmk_path, FS_O_READ);
	if (ret) {
		return;
	}

	ret = fs_read(&f, &bmk, sizeof(bmk));
	fs_close(&f);

	if (ret != sizeof(bmk) || memcmp(bmk.magic, NPB_BOOKMARK_MAGIC, sizeof(bmk.magic)) != 0) {
		return;
	}

	uint32_t row_index;

	if (npb_find_row(&book, bmk.last_position, &row_index) == 0) {
		page_start_row = row_index;
	}
}

static void turn_page(int direction)
{
	if (direction < 0) {
		if (page_start_row == 0) {
			return; /* already at the start of the book */
		}
		page_start_row = (page_start_row > content_rows) ? page_start_row - content_rows
								   : 0;
	} else {
		if (page_start_row + content_rows >= book.row_count) {
			return; /* already showing the last page */
		}
		page_start_row += content_rows;
	}

	/*
	 * One app_sd_init()/app_sd_deinit() bracket for the whole page turn -
	 * render_page() and save_bookmark() each open/close the book's file
	 * handle internally (see render_page()'s doc comment), but the SD
	 * card's own power/mount is held here at the coarser per-action
	 * granularity so it's paid once per turn, not twice.
	 */
	if (app_sd_init()) {
		printk("reader: SD unavailable, page turn not saved\n");
		return;
	}

	render_page();
	reader_flush();

	/* Cheap relative to a page turn's own ~530ms+ partial-refresh wait,
	 * and matches book-format.md's own "updated on every page turn"
	 * sidecar policy - see reader.h.
	 */
	save_bookmark();

	app_sd_deinit();
}

static void close_reader(void)
{
	void (*cb)(void) = exit_cb;

	if (app_sd_init() == 0) {
		save_bookmark();
		app_sd_deinit();
	}

	exit_cb = NULL;
	book_open = false;

	app_buttons_set_press_callback(NULL);
	app_buttons_set_long_press_callback(NULL);

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

/*
 * app_reader_menu_show()'s on_exit - reader_menu.h/toc.h both funnel
 * every exit path (its own "Back to Book" entry, and toc.c's own "Back"
 * *and* a successful chapter jump - see app_reader_jump_to_offset())
 * through this same callback, so there's exactly one place that restores
 * reading: re-register this screen's own button callbacks (the menu/toc
 * screens took both over - see open_reader_menu() below) and repaint the
 * current page, which the menu overwrote. Full refresh, not
 * reader_flush()'s partial-refresh counter, same reasoning as
 * app_reader_open()'s own first draw - this is a full screen-context
 * change, not an ordinary page turn.
 *
 * One app_sd_init()/app_sd_deinit() bracket around render_page() - same
 * reasoning as turn_page()/app_reader_open()'s identical brackets
 * (render_page()'s own npb_open() needs the card mounted). Whichever
 * menu/toc action got here (reader_menu.c's "Back to Book", toc.c's own
 * "Back", or app_reader_jump_to_offset()) already closed its own SD
 * session before calling this, so without this bracket render_page()'s
 * npb_open() fails silently and the framebuffer is left showing
 * whatever the menu/toc screen last drew - the app_text_full_refresh()
 * below then just redundantly re-flashes that stale content, while the
 * button-callback switch above still silently takes effect. That's
 * exactly the "Back to Book doesn't work until a second press" bug this
 * fixes.
 */
static void reopen_reading(void)
{
	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);

	if (app_sd_init()) {
		printk("reader: SD unavailable, can't redraw after menu/toc\n");
		return;
	}

	render_page();
	app_text_full_refresh();
	partial_count = 0;

	app_sd_deinit();
}

/*
 * buttons.h's long-press callback while a book is open - see reader.h's
 * top comment. Hands off button input to reader_menu.c entirely; nothing
 * further happens here until reopen_reading() (passed through as its
 * whole exit chain, however many menu/toc levels deep the user went)
 * hands it back.
 */
static void open_reader_menu(void)
{
	app_reader_menu_show(reopen_reading);
}

/*
 * Opens full_path_buf at reader_font and computes the content layout
 * (content_top_px/content_rows/reader_font_height) against whatever font
 * is currently active for the header/footer/browser - shared by
 * app_reader_open() (first open, position from the sidecar) and
 * app_reader_set_font() (font change mid-book, position carried over via
 * `resume_offset`/`have_resume_offset` instead of re-reading the sidecar).
 * Leaves `book` closed and returns a negative errno on any failure.
 */
static int open_book_at_font(uint32_t resume_offset, bool have_resume_offset)
{
	uint16_t grid_cols, grid_rows, chrome_row_height, reader_glyph_width, reader_cols;
	uint16_t content_bottom_px;
	int ret;

	ret = npb_open(full_path_buf, reader_font, &book);
	if (ret) {
		printk("reader: failed to open \"%s\" at font %d (%d)\n", full_path_buf,
		       reader_font, ret);
		return ret;
	}

	/* Content sits between the header row (row 0) and the footer row
	 * (the last row) of whatever font is CURRENTLY active for the rest
	 * of the UI - not reader_font itself, which only governs the content
	 * lines drawn between them (see reader_font's doc comment above and
	 * app_text_draw_text_px() in text.h).
	 */
	app_text_get_grid(&grid_cols, &grid_rows);
	if (grid_rows < 3) {
		/* Need at least the header row, one content row, and the
		 * footer row - same requirement browser.c's own
		 * app_browser_init() places on the active font.
		 */
		npb_close(&book);
		return -ENOSPC;
	}
	chrome_row_height = app_text_row_height();
	content_top_px = chrome_row_height;
	content_bottom_px = (uint16_t)(grid_rows - 1) * chrome_row_height;

	app_text_font_info(reader_font, &reader_glyph_width, &reader_font_height, &reader_cols);
	content_rows = (content_bottom_px - content_top_px) / reader_font_height;
	if (content_rows == 0) {
		npb_close(&book);
		return -ENOSPC;
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

int app_reader_open(const char *path, void (*on_exit)(void))
{
	int ret;

	/*
	 * One app_sd_init()/app_sd_deinit() bracket for the whole open
	 * sequence (resolve path, parse header/variant table, read the
	 * bookmark sidecar, render the first page) - see turn_page()'s
	 * identical reasoning. app_sd_full_path() itself needs the card
	 * already mounted (sd.h), hence starting the bracket before it.
	 */
	ret = app_sd_init();
	if (ret) {
		printk("reader: SD init failed (%d)\n", ret);
		return ret;
	}

	ret = app_sd_full_path(path, full_path_buf, sizeof(full_path_buf));
	if (ret) {
		printk("reader: couldn't resolve \"%s\" (%d)\n", path, ret);
		app_sd_deinit();
		return ret;
	}

	ret = snprintf(bmk_path, sizeof(bmk_path), "%s.bmk", full_path_buf);
	if (ret < 0 || (size_t)ret >= sizeof(bmk_path)) {
		printk("reader: bookmark sidecar path too long for \"%s\"\n", path);
		app_sd_deinit();
		return -ENAMETOOLONG;
	}

	ret = open_book_at_font(0, false);
	if (ret) {
		app_sd_deinit();
		return ret;
	}

	exit_cb = on_exit;
	partial_count = 0;
	book_open = true;

	render_page();

	ret = app_text_full_refresh();

	app_sd_deinit();

	if (ret) {
		exit_cb = NULL;
		book_open = false;
		return ret;
	}

	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);

	return 0;
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
	uint32_t row_index;
	int ret;

	if (!book_open) {
		return -EINVAL;
	}

	ret = app_sd_init();
	if (ret) {
		return ret;
	}

	ret = npb_open(full_path_buf, reader_font, &book);
	if (ret) {
		app_sd_deinit();
		return ret;
	}

	ret = npb_find_row(&book, text_offset, &row_index);
	npb_close(&book);

	if (ret) {
		app_sd_deinit();
		return ret;
	}

	page_start_row = row_index;
	save_bookmark();

	app_sd_deinit();

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

	/*
	 * One app_sd_init()/app_sd_deinit() bracket for the whole font-change
	 * sequence - see turn_page()'s identical reasoning. The book's file
	 * is closed between actions (render_page()'s doc comment), so it's
	 * reopened here just long enough to read the current position before
	 * switching variants.
	 */
	if (app_sd_init()) {
		printk("reader: SD init failed for font change\n");
		return;
	}

	if (npb_open(full_path_buf, old_font, &book) == 0) {
		resume_offset = current_blob_offset();
		npb_close(&book);
	}

	reader_font = font;

	if (open_book_at_font(resume_offset, true)) {
		/* The new font has no matching variant in this book (or some
		 * other failure) - fall back to the font that was already
		 * working rather than leaving the reader mid-book with no
		 * open file.
		 */
		printk("reader: font %d unavailable for this book, reverting to %d\n", font,
		       old_font);
		reader_font = old_font;
		if (open_book_at_font(resume_offset, true)) {
			/* Re-opening the font that was already working failed -
			 * shouldn't happen. Nothing left to recover into;
			 * leave the reader closed, same state as
			 * close_reader() left it in.
			 */
			book_open = false;
			app_buttons_set_press_callback(NULL);
			app_buttons_set_long_press_callback(NULL);
			app_sd_deinit();
			return;
		}
	}

	render_page();
	app_text_full_refresh();
	partial_count = 0;

	app_sd_deinit();
}
