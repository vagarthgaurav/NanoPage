/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "config.h"
#include "font_siji.h"
#include "header.h"
#include "npb.h"
#include "reader.h"
#include "sd.h"
#include "text.h"
#include "toc.h"

/* Row layout - same cursor/name convention as reader_menu.c (no
 * per-entry type icon; every entry here is either the synthetic "Back"
 * or a chapter, and neither needs one).
 */
#define APP_TOC_CURSOR_COL 0
#define APP_TOC_NAME_COL   3

/* Chapters + the synthetic "Back" entry pinned at index 0, capped so the
 * whole list fits in one static array rather than needing dynamic
 * allocation for an arbitrarily long chapter table - same reasoning as
 * APP_BROWSER_MAX_ENTRIES in browser.c. A book with more real chapters
 * than this just has its tail silently dropped from the list (still
 * fully readable by paging normally - this only limits jump targets).
 */
#define APP_TOC_MAX_CHAPTERS 63
#define APP_TOC_MAX_ENTRIES  (APP_TOC_MAX_CHAPTERS + 1)

/* Matches struct npb_chapter.title (npb_format.h) - NUL-padded on disk
 * but not guaranteed NUL-terminated if all 64 bytes are real title
 * characters, hence the +1 here.
 */
#define APP_TOC_TITLE_LEN 64

struct toc_entry {
	char title[APP_TOC_TITLE_LEN + 1];
	uint32_t text_offset;
	bool is_back;
};

static struct toc_entry entries[APP_TOC_MAX_ENTRIES];
static size_t entry_count;
static size_t selected_index;
static size_t scroll_offset;

/* See reader_menu.h's top comment - handed straight through from
 * app_reader_menu_show(), unchanged; toc.c never routes back up through
 * the menu that opened it.
 */
static void (*exit_cb)(void);

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;
static bool toc_active;

static void on_button(enum app_button button);

/*
 * Loads the currently-open book's chapter table into `entries`, brackets
 * its own app_sd_init()/app_sd_deinit() (same convention as browser.c's
 * load_dir()) - a fresh npb_open() handle, independent of reader.c's own
 * (which stays closed between actions - see reader.c's render_page()
 * doc comment). An empty/absent chapter table (book.chapter_count == 0)
 * isn't an error - entries[] just ends up with only the "Back" row.
 */
static int load_chapters(void)
{
	struct npb_book book;
	uint32_t count;
	int ret;

	strncpy(entries[0].title, "Back", sizeof(entries[0].title) - 1);
	entries[0].title[sizeof(entries[0].title) - 1] = '\0';
	entries[0].is_back = true;
	entries[0].text_offset = 0;
	entry_count = 1;

	ret = app_sd_init();
	if (ret) {
		return ret;
	}

	ret = npb_open(app_reader_get_path(), app_reader_get_font(), &book);
	if (ret) {
		app_sd_deinit();
		return ret;
	}

	count = MIN(book.chapter_count, (uint32_t)APP_TOC_MAX_CHAPTERS);

	for (uint32_t i = 0; i < count; i++) {
		struct npb_chapter chapter;

		if (npb_read_chapter(&book, i, &chapter)) {
			break;
		}

		strncpy(entries[entry_count].title, chapter.title,
			sizeof(entries[entry_count].title) - 1);
		entries[entry_count].title[sizeof(entries[entry_count].title) - 1] = '\0';
		entries[entry_count].text_offset = chapter.text_offset;
		entries[entry_count].is_back = false;
		entry_count++;
	}

	npb_close(&book);
	app_sd_deinit();

	selected_index = 0;
	scroll_offset = 0;

	return 0;
}

static void draw_name(uint16_t row, const char *name)
{
	size_t len = strlen(name);

	for (uint16_t col = APP_TOC_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_TOC_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? name[i] : ' ');
	}
}

/* `page_row` is 0-based within the visible content area - see browser.c's
 * identical convention for why (keeps scroll_offset math in one place).
 */
static void draw_entry(uint16_t page_row, size_t idx)
{
	uint16_t row = content_start_row + page_row;

	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	draw_name(row, entries[idx].title);
}

static void draw_cursor(uint16_t page_row, bool show)
{
	uint16_t row = content_start_row + page_row;

	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_TOC_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_TOC_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_TOC_CURSOR_COL + 1, ' ');
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

static void toc_flush(void)
{
	partial_count++;

	if (partial_count >= APP_TOC_FULL_REFRESH_EVERY) {
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
		scroll_offset = (new_index < scroll_offset) ? new_index
							      : new_index - content_rows + 1;
		selected_index = new_index;
		redraw_page();
	} else {
		old_page_row = (uint16_t)(selected_index - scroll_offset);
		draw_cursor(old_page_row, false);
		selected_index = new_index;
		draw_cursor((uint16_t)(selected_index - scroll_offset), true);
	}

	toc_flush();
}

static void select_entry(void)
{
	void (*cb)(void) = exit_cb;

	if (entry_count == 0) {
		return;
	}

	if (!entries[selected_index].is_back) {
		int ret = app_reader_jump_to_offset(entries[selected_index].text_offset);

		if (ret) {
			printk("toc: failed to jump to chapter (%d)\n", ret);
			/* Position unchanged (see app_reader_jump_to_offset()'s
			 * own doc comment) - still leave the screen via the
			 * same exit path rather than stranding the user here.
			 */
		}
	}

	toc_active = false;
	app_buttons_set_press_callback(NULL);

	if (cb) {
		cb();
	}
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
		select_entry();
		break;
	}
}

void app_toc_show(void (*on_exit)(void))
{
	uint16_t rows;

	exit_cb = on_exit;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	/* A failed load still shows the screen with just "Back" (entries[0]
	 * is always set before load_chapters() can fail) rather than
	 * leaving the user stuck with no button callback at all.
	 */
	load_chapters();

	redraw_page();
	app_header_update("Table of Contents");

	/* Always a full refresh - opening this screen is an infrequent,
	 * deliberate transition, same reasoning as browser.c's own
	 * folder-navigation full refresh.
	 */
	app_text_full_refresh();
	partial_count = 0;

	app_buttons_set_press_callback(on_button);
	toc_active = true;
}

bool app_toc_is_active(void)
{
	return toc_active;
}
