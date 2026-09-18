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

/*
 * Sidecar files live in a hidden subfolder of the book's own directory
 * (e.g. a book at "/books/foo.npb" gets a sidecar at
 * "/books/.nanopage/foo.npb.bmk") rather than directly alongside it, so
 * they don't clutter the file browser's listing with per-book bookkeeping
 * the user has no reason to see or delete - previously they sat right
 * next to the book as "<book>.npb.bmk" and were only hidden from the
 * browser by an explicit ".bmk" extension filter (browser.c's
 * has_bmk_extension()). "Hidden" here is only the FAT/dot-prefix sense
 * app_sd_list_dir() already filters on (sd.c's fs_readdir() loop skips
 * any entry whose name starts with '.') - there's no FAT hidden file
 * attribute involved, and no on-device migration of pre-existing
 * "<book>.npb.bmk" sidecars from before this folder existed.
 */
#define APP_READER_BMK_DIR ".nanopage"

static struct npb_book book;
static char full_path_buf[APP_SD_MAX_PATH_LEN];
/* The SD-root-relative path app_reader_open() was actually called with
 * (e.g. "/books/foo.npb") - unlike full_path_buf above (the mount-prefixed
 * form fs_open() etc. need), this is what gets persisted to the session
 * marker (see flush_pending_state()/app_reader_resume_last_session()
 * below), since that's the same form app_reader_open() itself expects back.
 */
static char session_path_buf[APP_SD_MAX_PATH_LEN];
/* Mount-prefixed path of the sidecar's containing hidden folder (e.g.
 * "/SD:/books/.nanopage") - built alongside bmk_path below so
 * app_reader_open() can fs_mkdir() it before the first save.
 */
static char bmk_dir_buf[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 4];
/* Mount-prefixed sidecar file path (e.g. "/SD:/books/.nanopage/foo.npb.bmk") -
 * margin above bmk_dir_buf's own size covers "/" + the book's base filename
 * (bounded by APP_SD_MAX_NAME_LEN, same as everywhere else a single SD
 * entry name is held) + ".bmk" + NUL.
 */
static char bmk_path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + APP_SD_MAX_NAME_LEN + 8];
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

/*
 * The whole bookmark sidecar, held in RAM for the duration of a reading
 * session (loaded once in load_bookmark(), on the book's first open -
 * NOT reloaded on a font switch, so bookmarks survive that, same as
 * last_position already did). save_bookmark() always writes this struct
 * back wholesale, so toggle_bookmark() can add/remove a slot with plain
 * in-memory array surgery and no separate read-modify-write of the file.
 * bookmarks.c reads a copy of bookmarks[0..bookmark_count) via
 * app_reader_get_bookmarks() - see reader.h.
 */
static struct npb_bookmark_file bmk_state;

/* Whether the page currently on screen has a bookmark on it - recomputed
 * only when the displayed page actually changes (render_page(), which
 * already has the book open to read row data, so this costs nothing
 * extra there), not on every app_reader_current_page_bookmarked() call -
 * see that function's own doc comment in reader.h.
 */
static bool page_bookmarked;

static uint8_t partial_count;

/* Index into bmk_state.bookmarks[] of the bookmark at `offset`, or -1 if
 * none - a bookmark's text_offset is always an exact page-start blob
 * offset (see toggle_bookmark()), so an exact match is the right test
 * (not a range/containment check).
 */
static int bookmark_index_for_offset(uint32_t offset)
{
	for (uint8_t i = 0; i < bmk_state.bookmark_count; i++) {
		if (bmk_state.bookmarks[i].text_offset == offset) {
			return i;
		}
	}

	return -1;
}

/* Forward-declared: current_blob_offset()'s own definition sits right
 * after render_page() (its doc comment explains why - it's the natural
 * continuation of "what render_page() just drew"), but render_page()
 * itself now needs it too, to compute page_bookmarked while the file's
 * already open.
 */
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

	/* Cheap here - book is already open and current_blob_offset() is
	 * just one more row read - see page_bookmarked's own doc comment
	 * above for why this is the one place that recomputes it.
	 */
	page_bookmarked = bookmark_index_for_offset(current_blob_offset()) >= 0;

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

/*
 * Writes bmk_state wholesale to the sidecar - the whole struct, bookmarks
 * array included, per npb_bookmark_format.h's "fixed-size, always
 * rewritten in one shot" design. Refreshes last_position/last_font_id
 * from the current reading position first; bmk_state.bookmarks[]/
 * bookmark_count are left exactly as toggle_bookmark() (or load_bookmark()
 * on first load) last set them - this function never touches bookmark
 * content itself, only the position/font fields every save needs to
 * update regardless of whether a bookmark changed.
 */
static void save_bookmark(void)
{
	struct fs_file_t f;
	uint32_t last_position = 0;
	int ret;

	if (npb_open(full_path_buf, reader_font, &book) == 0) {
		last_position = current_blob_offset();
		npb_close(&book);
	}

	memcpy(bmk_state.magic, NPB_BOOKMARK_MAGIC, sizeof(bmk_state.magic));
	bmk_state.version = NPB_BOOKMARK_VERSION;
	bmk_state.last_font_id = book.font_id;
	bmk_state.last_position = last_position;

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

/*
 * SD-root-relative folder + fixed filename for the "resume this on the next
 * boot" marker - lives in the SD root's own ".nanopage" folder (the same
 * hidden-folder convention build_bmk_paths() uses per-book-directory, just
 * rooted at "/" instead of a book's own directory), NOT the per-book
 * bookmark sidecar's globals (bmk_dir_buf/bmk_path) - reusing those here
 * would clobber the currently-open book's own bookmark path out from under
 * save_bookmark(). "session" can't collide with a real bookmark sidecar's
 * name, which is always "<book filename>.bmk" (build_bmk_paths()).
 */
#define APP_READER_SESSION_DIR_REL "/" APP_READER_BMK_DIR
#define APP_READER_SESSION_FILE_NAME "session"

/*
 * Resolves the mount-prefixed path of the session marker file into `out` -
 * shared by flush_pending_state() and app_reader_resume_last_session().
 * `create_dir` additionally fs_mkdir()s the containing ".nanopage" folder
 * first (best-effort, same as app_reader_open()'s identical call for a
 * book's own bookmark folder) - needed before writing the marker, since
 * nothing may have created it yet if no book at the SD root itself has
 * ever been bookmarked; skipped when only reading, since a missing folder
 * there just means fs_open() below fails with nothing to resume, the same
 * as a missing file would. Requires the card to already be mounted, same
 * as app_sd_full_path().
 */
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

/*
 * Registered with power.c as its idle hook (app_power_set_idle_hook()) for
 * as long as a book is open - see that function's own doc comment for why
 * this indirection exists instead of power.c calling into this file
 * directly. Two independent jobs, both about not losing state to an
 * idle-triggered System OFF, which is the only thing that ever calls this
 * (idle_work is power.c's sole caller of enter_system_off()) - so this is
 * also the one place that needs to keep the session marker below in sync,
 * nothing else has to touch it:
 *
 *   1. save_bookmark() - every reader mutation that changes bmk_state
 *      already calls this itself before returning (turn_page(),
 *      toggle_bookmark(), app_reader_jump_to_offset()) except
 *      app_reader_set_font(), which only picks up book.font_id on the
 *      *next* save (see its own doc comment) - this is the safety net for
 *      that (and any future case like it).
 *
 *   2. the session marker (session_marker_path() above) - written with
 *      the currently-open book's path if book_open, or removed if not, so
 *      app_reader_resume_last_session() (called from main.c after a
 *      System-OFF wake) knows whether - and what - to reopen instead of
 *      booting to the app menu. Removed rather than left stale when no
 *      book is open, so closing the reader (close_reader()) and then
 *      idling out doesn't incorrectly resume the book the user already
 *      walked away from.
 */
static void flush_pending_state(void)
{
	char path[APP_SD_MAX_PATH_LEN + sizeof(APP_READER_BMK_DIR) + 24];

	if (app_sd_init()) {
		printk("reader: SD unavailable, pending state not flushed before sleep\n");
		return;
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
	} else if (session_marker_path(path, sizeof(path), false) == 0) {
		fs_unlink(path);
	}

	app_sd_deinit();
}

/*
 * Loads the sidecar (if any) into bmk_state wholesale - both the
 * auto-resume position (resolved to a starting row via npb_find_row(),
 * book-format.md's binary-search primitive) and every explicit bookmark,
 * which stay resident in bmk_state for the rest of the reading session
 * (see bmk_state's own doc comment above). Leaves page_start_row at 0
 * and bmk_state at a fresh, empty-bookmarks default if there's no
 * sidecar yet, or if anything about reading it fails - never treated as
 * a fatal error, an unreadable/missing/corrupt sidecar just means
 * "nothing to resume, no bookmarks yet," not a reason to fail opening
 * the book.
 */
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

	ret = fs_read(&f, &on_disk, sizeof(on_disk));
	fs_close(&f);

	if (ret != sizeof(on_disk) ||
	    memcmp(on_disk.magic, NPB_BOOKMARK_MAGIC, sizeof(on_disk.magic)) != 0) {
		return;
	}

	bmk_state = on_disk;
	if (bmk_state.bookmark_count > NPB_MAX_BOOKMARKS) {
		/* Shouldn't happen from anything this firmware ever wrote,
		 * but a corrupt/foreign file could claim more than the
		 * array actually holds - clamp defensively rather than
		 * reading past bmk_state.bookmarks[] later.
		 */
		bmk_state.bookmark_count = NPB_MAX_BOOKMARKS;
	}

	uint32_t row_index;

	if (npb_find_row(&book, bmk_state.last_position, &row_index) == 0) {
		page_start_row = row_index;
	}
}

/*
 * The Prev+Action combo handler (buttons.h's app_buttons_set_combo_
 * callback()) - see reader.h's top comment. Adds a bookmark at the
 * current page's blob offset if none exists there yet (up to
 * NPB_MAX_BOOKMARKS), removes it if one already does. Brackets its own
 * app_sd_init()/app_sd_deinit() pair, same convention as turn_page()/
 * app_reader_open() - a fresh, short-lived npb_open() just to read the
 * current page's own blob offset (render_page() already computed
 * page_bookmarked for the boolean question, but toggling needs the
 * actual offset value: to store when adding, or to find the matching
 * slot's index when removing).
 */
static void toggle_bookmark(void)
{
	uint32_t offset;
	int idx;

	if (!book_open) {
		return;
	}

	if (app_sd_init()) {
		printk("reader: SD unavailable, bookmark not toggled\n");
		return;
	}

	if (npb_open(full_path_buf, reader_font, &book) != 0) {
		app_sd_deinit();
		return;
	}
	offset = current_blob_offset();
	npb_close(&book);

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
		app_sd_deinit();
		return;
	}

	save_bookmark();
	app_sd_deinit();

	if (app_header_update("", page_bookmarked) > 0) {
		app_text_flush();
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

	/* Folded into the same flush as the page redraw below (text.c
	 * coalesces adjacent dirty rows into one partial-refresh window),
	 * so the header's bookmark icon updates in lockstep with turning
	 * onto/off of a bookmarked page instead of lagging until the next
	 * once-a-minute RTC tick.
	 */
	app_header_update("", page_bookmarked);

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
	app_buttons_set_combo_callback(NULL);
	app_power_set_idle_hook(NULL);

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
 * app_reader_menu_show()'s on_exit - the callback reader.c hands to the
 * in-book menu, which the menu then threads onward as reader_menu.c's
 * own `exit_cb`: its own "Back to Book" entry calls this directly, and
 * it's also what toc.c/bookmarks.c call as their `on_selected` when the
 * user actually picks a chapter/bookmark (see app_reader_jump_to_offset()) -
 * so there's exactly one place that restores reading: re-register this
 * screen's own button callbacks (the menu/toc/bookmarks screens took
 * them over - see open_reader_menu() below) and repaint the current
 * page, which whichever screen was showing overwrote. Full refresh, not
 * reader_flush()'s partial-refresh counter, same reasoning as
 * app_reader_open()'s own first draw - this is a full screen-context
 * change, not an ordinary page turn.
 *
 * Not the same as toc.c/bookmarks.c's own "Back" entry, which returns to
 * the in-book menu instead (reader_menu.c's return_to_menu()), not all
 * the way here - see toc.h/bookmarks.h's doc comments on their
 * on_back/on_selected split.
 *
 * One app_sd_init()/app_sd_deinit() bracket around render_page() - same
 * reasoning as turn_page()/app_reader_open()'s identical brackets
 * (render_page()'s own npb_open() needs the card mounted). Whichever
 * action got here (reader_menu.c's "Back to Book", or a toc.c/
 * bookmarks.c jump via app_reader_jump_to_offset()) already closed its
 * own SD session before calling this, so without this bracket
 * render_page()'s npb_open() fails silently and the framebuffer is left
 * showing whatever screen last drew - the app_text_full_refresh() below
 * then just redundantly re-flashes that stale content, while the
 * button-callback switch above still silently takes effect. That's
 * exactly the "Back to Book doesn't work until a second press" bug this
 * fixes.
 */
static void reopen_reading(void)
{
	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);
	app_buttons_set_combo_callback(toggle_bookmark);

	if (app_sd_init()) {
		printk("reader: SD unavailable, can't redraw after menu/toc\n");
		return;
	}

	render_page();
	app_header_update("", page_bookmarked);
	app_text_full_refresh();
	partial_count = 0;

	app_sd_deinit();
}

/*
 * buttons.h's long-press callback while a book is open - see reader.h's
 * top comment. Hands off button input to reader_menu.c entirely; nothing
 * further happens here until reopen_reading() (passed through as its
 * whole exit chain, however many menu/toc levels deep the user went)
 * hands it back. Clears the combo callback too - the Prev+Action
 * bookmark toggle only makes sense while an actual page is on screen,
 * not while reader_menu.c/toc.c/bookmarks.c own button input (which
 * reopen_reading() above re-registers on the way back).
 */
static void open_reader_menu(void)
{
	app_buttons_set_combo_callback(NULL);
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

/*
 * Fills bmk_dir_buf/bmk_path from `path` (the book's own SD-root-relative
 * path, e.g. "/books/foo.npb") - see APP_READER_BMK_DIR's doc comment
 * above for the folder layout. Requires the card to already be mounted
 * (app_sd_full_path()'s own requirement). Returns 0 on success, negative
 * errno if any piece doesn't fit its buffer.
 */
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

	/* Copied immediately, not just referenced - `path` is often a
	 * caller's own stack buffer (e.g. browser.c's enter_selected()) that
	 * won't outlive this call, but flush_pending_state() needs it to
	 * still be valid for as long as the book stays open (see
	 * session_path_buf's own doc comment above).
	 */
	strncpy(session_path_buf, path, sizeof(session_path_buf) - 1);
	session_path_buf[sizeof(session_path_buf) - 1] = '\0';

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

	ret = build_bmk_paths(path);
	if (ret) {
		printk("reader: bookmark sidecar path too long for \"%s\"\n", path);
		app_sd_deinit();
		return ret;
	}

	/*
	 * Best-effort - create the hidden sidecar folder if it doesn't exist
	 * yet (first time this book's directory has ever had a bookmark
	 * saved). -EEXIST is the expected, common case and not logged;
	 * anything else just means save_bookmark()'s later fs_open(FS_O_
	 * CREATE) will fail too and the position won't persist - same
	 * degrade-gracefully behavior as every other SD failure in this app,
	 * not fatal to opening the book itself.
	 */
	ret = fs_mkdir(bmk_dir_buf);
	if (ret && ret != -EEXIST) {
		printk("reader: couldn't create bookmark sidecar folder \"%s\" (%d)\n", bmk_dir_buf,
		       ret);
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
	app_header_update("", page_bookmarked);

	ret = app_text_full_refresh();

	app_sd_deinit();

	if (ret) {
		exit_cb = NULL;
		book_open = false;
		return ret;
	}

	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(open_reader_menu);
	app_buttons_set_combo_callback(toggle_bookmark);
	app_power_set_idle_hook(flush_pending_state);

	return 0;
}

/*
 * main.c's boot-time hook, called once after a System-OFF wake
 * (app_power_woke_from_system_off()) - reads the session marker
 * flush_pending_state() leaves behind and, if one names a book, reopens it
 * exactly like a normal app_reader_open() call (`on_exit` is threaded
 * straight through, same convention). Returns 0 if a session was found and
 * successfully reopened; a negative errno otherwise (no marker, no SD
 * card, or the book itself failed to open) - the caller is expected to
 * fall back to app_menu_show() in that case, same as a cold boot.
 *
 * Deliberately its own short app_sd_init()/app_sd_deinit() bracket just to
 * read the marker, released *before* calling app_reader_open() rather than
 * held across it - app_sd_init() isn't safe to call again while already
 * mounted (see sd.h), so nesting the two brackets isn't an option here the
 * way it is for a single fs_open()/fs_read() pair elsewhere in this file.
 */
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
			app_buttons_set_combo_callback(NULL);
			app_power_set_idle_hook(NULL);
			app_sd_deinit();
			return;
		}
	}

	render_page();
	app_header_update("", page_bookmarked);
	app_text_full_refresh();
	partial_count = 0;

	app_sd_deinit();
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
