/* SPDX-License-Identifier: Apache-2.0 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zephyr/kernel.h>

#include "browser.h"
#include "buttons.h"
#include "config.h"
#include "font_siji.h"
#include "header.h"
#include "reader.h"
#include "sd.h"
#include "text.h"

/*
 * Row layout, in text-grid columns (all icons are exactly 2 Tamzen cells
 * wide - see header.c's APP_HEADER_ICON_COLS for the same 12px-icon/6px-
 * cell relationship):
 *   col 0-1: cursor icon (blank unless this is the selected row)
 *   col 2-3: folder/file type icon
 *   col 4:   gap
 *   col 5..: name, truncated to whatever's left
 */
#define APP_BROWSER_CURSOR_COL     0
#define APP_BROWSER_TYPE_ICON_COL  2
#define APP_BROWSER_NAME_COL       5

/* Directory + optional synthetic ".." entry, capped so the whole listing
 * fits in one static array rather than needing dynamic allocation for an
 * arbitrarily large folder - see APP_SD_MAX_NAME_LEN's doc comment in
 * sd.h for the same reasoning applied to a single name.
 */
#define APP_BROWSER_MAX_ENTRIES 64

/* APP_BROWSER_FULL_REFRESH_EVERY (config.h): forces a full-panel refresh
 * every this-many partial-refresh-triggering actions (cursor moves and
 * page scrolls - NOT folder navigation, which always gets its own full
 * refresh regardless of this counter) to clear ghosting - see browser.h's
 * top comment.
 */

struct browser_entry {
	char name[APP_SD_MAX_NAME_LEN];
	bool is_dir;
	bool is_parent; /* synthetic ".." - always pinned at index 0 when present */
};

static struct browser_entry entries[APP_BROWSER_MAX_ENTRIES];
static size_t entry_count;
static size_t selected_index;
static size_t scroll_offset;

static char current_path[APP_SD_MAX_PATH_LEN] = "/";

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;

static bool browser_ready;

/* Called when the synthetic ".." entry at the SD root is selected - see
 * browser.h. Set once by app_browser_init(); app_browser_reinit() (usb_msc.c)
 * deliberately leaves it untouched, since a USB reconnect resumes the same
 * browser session rather than starting a fresh one.
 */
static void (*exit_cb)(void);

static void on_button(enum app_button button);

/* .bmk files are per-book reader-progress sidecars (reader.c) - purely
 * internal bookkeeping, never something the user opens directly. They now
 * live inside a hidden ".nanopage" subfolder next to each book (see
 * reader.c's APP_READER_BMK_DIR), which app_sd_list_dir() already omits
 * from every listing on its own (sd.c skips any dot-prefixed entry), so
 * this extension filter is only a defensive leftover in case an older
 * sidecar (from before that folder existed) is still sitting directly
 * next to a book on some card.
 */
static bool has_bmk_extension(const char *name)
{
	size_t len = strlen(name);

	return len > 4 && strcasecmp(name + len - 4, ".bmk") == 0;
}

/*
 * load_dir()'s scratch buffer for app_sd_list_dir() - deliberately static/
 * BSS rather than a local variable: at APP_BROWSER_MAX_ENTRIES entries
 * this is ~3.3KB, and load_dir() now runs from the button-press path on
 * the system workqueue thread (buttons.h), not main(). prj.conf's own
 * CONFIG_MAIN_STACK_SIZE comment already documents this exact FATFS call
 * chain (fs_opendir()/fs_readdir() -> ff.c -> the SDMMC/SPI driver)
 * blowing a 1024-byte stack on main() with far less of its own added on
 * top - putting this on the stack here, on a workqueue thread bumped to
 * the same size (see CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE in prj.conf),
 * would reintroduce that MPU stacking fault immediately.
 */
static struct app_sd_dirent raw_listing[APP_BROWSER_MAX_ENTRIES];

/* Directories before files, then case-insensitive alphabetical within
 * each group - the synthetic ".." entry (if any) is excluded from this
 * (see load_dir()'s `start` argument), so it always stays pinned first
 * regardless of what it'd sort to as a plain name.
 */
static int entry_cmp(const void *a, const void *b)
{
	const struct browser_entry *ea = a;
	const struct browser_entry *eb = b;

	if (ea->is_dir != eb->is_dir) {
		return ea->is_dir ? -1 : 1;
	}

	return strcasecmp(ea->name, eb->name);
}

static int load_dir(const char *path)
{
	size_t raw_count = 0;
	size_t start = 0;
	int ret;

	entry_count = 0;

	/*
	 * A synthetic ".." entry: goes up a directory everywhere except the
	 * SD root, where there's nowhere further "up" on the card itself -
	 * there it instead calls exit_cb (see enter_selected()) to leave the
	 * browser for whoever launched it. Omitted entirely at the root with
	 * no exit_cb (e.g. no caller to return to), same as before this
	 * behavior existed.
	 */
	if (strcmp(path, "/") != 0 || exit_cb != NULL) {
		strncpy(entries[0].name, "..", sizeof(entries[0].name) - 1);
		entries[0].name[sizeof(entries[0].name) - 1] = '\0';
		entries[0].is_dir = true;
		entries[0].is_parent = true;
		entry_count = 1;
		start = 1;
	}

	/*
	 * Brackets the SD card's power/mount for just this one listing -
	 * see sd.h's own note that a browser navigating folder-to-folder
	 * should call app_sd_init()/app_sd_deinit() around each listing
	 * rather than holding one long-lived mount, so the card is powered
	 * only for the moment it's actually being read (plus whatever a
	 * display refresh separately needs via display.c's own bracket).
	 */
	ret = app_sd_init();
	if (ret) {
		return ret;
	}

	ret = app_sd_list_dir(path, raw_listing, APP_BROWSER_MAX_ENTRIES - start, &raw_count);

	app_sd_deinit();

	if (ret) {
		return ret;
	}

	for (size_t i = 0; i < raw_count; i++) {
		if (!raw_listing[i].is_dir && has_bmk_extension(raw_listing[i].name)) {
			continue;
		}

		strncpy(entries[entry_count].name, raw_listing[i].name,
			sizeof(entries[entry_count].name) - 1);
		entries[entry_count].name[sizeof(entries[entry_count].name) - 1] = '\0';
		entries[entry_count].is_dir = raw_listing[i].is_dir;
		entries[entry_count].is_parent = false;
		entry_count++;
	}

	if (entry_count > start + 1) {
		qsort(&entries[start], entry_count - start, sizeof(entries[0]), entry_cmp);
	}

	selected_index = 0;
	scroll_offset = 0;

	return 0;
}

static void draw_name(uint16_t row, const char *name)
{
	size_t len = strlen(name);

	for (uint16_t col = APP_BROWSER_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_BROWSER_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? name[i] : ' ');
	}
}

/* `page_row` is 0-based within the visible content area, not a raw grid
 * row - every drawing function here takes/returns page-relative rows so
 * scroll_offset math only ever happens in one place (move_cursor()).
 */
static void draw_entry(uint16_t page_row, size_t idx)
{
	uint16_t row = content_start_row + page_row;

	/* Blank the whole row first so a shorter new name (or no entry at
	 * all, past the end of a short directory) fully replaces whatever
	 * a longer previous one left behind.
	 */
	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	app_text_draw_icon(row, app_text_col_to_px(APP_BROWSER_TYPE_ICON_COL),
			    entries[idx].is_dir ? APP_TEXT_ICON_FOLDER : APP_TEXT_ICON_FILE);
	draw_name(row, entries[idx].name);
}

static void draw_cursor(uint16_t page_row, bool show)
{
	uint16_t row = content_start_row + page_row;

	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_BROWSER_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_BROWSER_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_BROWSER_CURSOR_COL + 1, ' ');
	}
}

static void redraw_page(void)
{
	for (uint16_t i = 0; i < content_rows; i++) {
		draw_entry(i, scroll_offset + i);
	}

	if (entry_count > 0) {
		draw_cursor((uint16_t)(selected_index - scroll_offset), true);
	}
}

/* Every caller of this already did its own text.c drawing; this just
 * decides partial vs. full and keeps the shared counter - see
 * APP_BROWSER_FULL_REFRESH_EVERY above.
 */
static void browser_flush(void)
{
	partial_count++;

	if (partial_count >= APP_BROWSER_FULL_REFRESH_EVERY) {
		app_text_full_refresh();
		partial_count = 0;
	} else {
		app_text_flush();
	}
}

static void move_cursor(int delta)
{
	size_t new_index;
	uint16_t old_page_row;

	if (entry_count == 0) {
		return;
	}

	if (delta < 0) {
		if (selected_index == 0) {
			return;
		}
		new_index = selected_index - 1;
	} else {
		if (selected_index + 1 >= entry_count) {
			return;
		}
		new_index = selected_index + 1;
	}

	if (new_index < scroll_offset || new_index >= scroll_offset + content_rows) {
		/* Scrolled off the visible page - the whole page's contents
		 * shift, not just the cursor, so every row needs redrawing.
		 */
		scroll_offset = (new_index < scroll_offset) ? new_index
							      : new_index - content_rows + 1;
		selected_index = new_index;
		redraw_page();
	} else {
		/* Same page: only the two affected rows' cursor column
		 * actually changed - the cheapest possible partial refresh.
		 */
		old_page_row = (uint16_t)(selected_index - scroll_offset);
		draw_cursor(old_page_row, false);
		selected_index = new_index;
		draw_cursor((uint16_t)(selected_index - scroll_offset), true);
	}

	browser_flush();
}

/* Shared by both the "descend into a directory" and "open a file" paths
 * below - current_path is always '/'-terminated or the SD root ("/") itself.
 */
static int build_child_path(const char *name, char *out, size_t out_size)
{
	size_t path_len = strlen(current_path);
	int ret;

	ret = snprintf(out, out_size, "%s%s%s", current_path,
			(path_len > 0 && current_path[path_len - 1] == '/') ? "" : "/", name);
	if (ret < 0 || (size_t)ret >= out_size) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static bool has_npb_extension(const char *name)
{
	size_t len = strlen(name);

	return len > 4 && strcasecmp(name + len - 4, ".npb") == 0;
}

/*
 * app_reader_open()'s on_exit callback for a book opened from here (see the
 * plain-file branch of enter_selected() below) - re-registers the
 * browser's own button callback and redraws its listing. The directory
 * hasn't changed while a book was open, so this is just the same
 * redraw/flush app_browser_init() itself already does at the end of its
 * own setup, without paying for a fresh load_dir()/remount.
 */
static void resume_browser(void)
{
	app_buttons_set_press_callback(on_button);
	redraw_page();
	app_header_update(current_path, false);
	app_text_full_refresh();
	partial_count = 0;
}

static void enter_selected(void)
{
	char new_path[APP_SD_MAX_PATH_LEN];
	int ret;

	if (entry_count == 0) {
		return;
	}

	if (!entries[selected_index].is_dir) {
		if (!has_npb_extension(entries[selected_index].name)) {
			return; /* not a recognized book format - nothing to do */
		}

		ret = build_child_path(entries[selected_index].name, new_path, sizeof(new_path));
		if (ret) {
			printk("browser: path too long, not opening \"%s\"\n",
			       entries[selected_index].name);
			return;
		}

		ret = app_reader_open(new_path, resume_browser);
		if (ret) {
			printk("browser: failed to open \"%s\" (%d)\n", new_path, ret);
		}
		return;
	}

	if (entries[selected_index].is_parent) {
		if (strcmp(current_path, "/") == 0) {
			/* Nowhere further "up" on the card itself - hand
			 * control back to whoever launched the browser
			 * instead. Same shutdown shape as reader.c's
			 * close_reader(): stop taking button input and mark
			 * ourselves not-ready before calling out, in case the
			 * callback (or whatever it does) checks either.
			 */
			void (*cb)(void) = exit_cb;

			app_buttons_set_press_callback(NULL);
			browser_ready = false;

			if (cb) {
				cb();
			}
			return;
		}

		char *slash = strrchr(current_path, '/');

		if (slash == NULL || slash == current_path) {
			strcpy(new_path, "/");
		} else {
			size_t len = (size_t)(slash - current_path);

			memcpy(new_path, current_path, len);
			new_path[len] = '\0';
		}
	} else {
		ret = build_child_path(entries[selected_index].name, new_path, sizeof(new_path));
		if (ret) {
			printk("browser: path too long, not entering \"%s\"\n",
			       entries[selected_index].name);
			return;
		}
	}

	ret = load_dir(new_path);
	if (ret) {
		printk("browser: failed to list \"%s\" (%d)\n", new_path, ret);
		return;
	}

	strncpy(current_path, new_path, sizeof(current_path) - 1);
	current_path[sizeof(current_path) - 1] = '\0';

	redraw_page();
	/* Folded into the same flush as the page redraw below, so the
	 * header's folder text and the new listing land on screen together
	 * instead of the header lagging until the next RTC-minute tick.
	 */
	app_header_update(current_path, false);

	/*
	 * Always a full refresh, not routed through browser_flush()'s
	 * partial-refresh counter - entering/leaving a folder is a rare,
	 * deliberate navigation action (unlike a cursor move or same-page
	 * scroll, which happen constantly), so paying the full flashing
	 * refresh here both keeps the listing ghost-free right when it
	 * matters most (a whole new set of names on screen) and resets the
	 * counter, giving every subsequent run of cursor moves a full
	 * APP_BROWSER_FULL_REFRESH_EVERY partials before its own next one.
	 */
	app_text_full_refresh();
	partial_count = 0;
}

static void on_button(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		move_cursor(-1);
		break;
	case APP_BUTTON_NEXT:
		move_cursor(1);
		break;
	case APP_BUTTON_ACTION:
		enter_selected();
		break;
	}
}

/* Shared by app_browser_init()/app_browser_reinit() - see browser.h for why
 * exit_cb isn't touched here (it's the one thing that differs between the
 * two callers).
 */
static int browser_open(const char *root)
{
	uint16_t rows;
	int ret;

	browser_ready = false;

	app_text_get_grid(&grid_cols, &rows);
	if (rows < 3) {
		/* Need at least the header row, one content row, and the
		 * footer row.
		 */
		return -ENOSPC;
	}
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = rows - 2;

	strncpy(current_path, root, sizeof(current_path) - 1);
	current_path[sizeof(current_path) - 1] = '\0';

	ret = load_dir(current_path);
	if (ret) {
		printk("browser: failed to list \"%s\" (%d)\n", current_path, ret);
		return ret;
	}

	redraw_page();
	app_header_update(current_path, false);

	/* First draw - full refresh for a clean, ghost-free start, same as
	 * main.c's own boot-time app_text_full_refresh() call.
	 */
	ret = app_text_full_refresh();
	if (ret) {
		return ret;
	}
	partial_count = 0;

	app_buttons_set_press_callback(on_button);

	browser_ready = true;

	return 0;
}

int app_browser_init(const char *root, void (*on_exit)(void))
{
	exit_cb = on_exit;

	return browser_open(root);
}

int app_browser_reinit(void)
{
	return browser_open(current_path);
}

bool app_browser_is_ready(void)
{
	return browser_ready;
}

const char *app_browser_get_current_path(void)
{
	return current_path;
}
