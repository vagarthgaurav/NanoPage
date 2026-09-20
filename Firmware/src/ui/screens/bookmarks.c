/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "bookmarks.h"
#include "buttons.h"
#include "config.h"
#include "header.h"
#include "npb.h"
#include "npb_bookmark_format.h"
#include "reader.h"
#include "sd.h"
#include "text.h"

#define APP_BOOKMARKS_CURSOR_COL 0
#define APP_BOOKMARKS_NAME_COL   3

#define APP_BOOKMARKS_MAX_ENTRIES (NPB_MAX_BOOKMARKS + 1)

#define APP_BOOKMARKS_TEXT_LEN 96

#define APP_BOOKMARKS_CHAPTER_TITLE_LEN 64

struct bookmarks_entry {
	char text[APP_BOOKMARKS_TEXT_LEN];
	uint32_t text_offset;
	bool is_back;
};

static struct bookmarks_entry entries[APP_BOOKMARKS_MAX_ENTRIES];
static size_t entry_count;
static size_t selected_index;
static size_t scroll_offset;

static void (*on_back_cb)(void);
static void (*on_selected_cb)(void);

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;
static bool bookmarks_active;

static void on_button(enum app_button button);

static bool chapter_title_for_offset(struct npb_book *book, uint32_t offset, char *out,
				      size_t out_size)
{
	bool found = false;

	for (uint32_t i = 0; i < book->chapter_count; i++) {
		struct npb_chapter chapter;

		if (npb_read_chapter(book, i, &chapter)) {
			break;
		}

		if (chapter.text_offset > offset) {
			break;
		}

		strncpy(out, chapter.title, out_size - 1);
		out[out_size - 1] = '\0';
		found = true;
	}

	return found;
}

static int load_bookmarks(void)
{
	struct npb_bookmark raw[NPB_MAX_BOOKMARKS];
	struct npb_book book;
	size_t raw_count = 0;
	uint16_t content_rows_per_page = app_reader_get_content_rows();
	bool book_opened = false;

	strncpy(entries[0].text, "Back", sizeof(entries[0].text) - 1);
	entries[0].text[sizeof(entries[0].text) - 1] = '\0';
	entries[0].is_back = true;
	entries[0].text_offset = 0;
	entry_count = 1;

	if (app_reader_get_bookmarks(raw, ARRAY_SIZE(raw), &raw_count) || raw_count == 0) {
		selected_index = 0;
		scroll_offset = 0;
		return 0;
	}

	if (app_sd_init() == 0) {
		book_opened = (npb_open(app_reader_get_path(), app_reader_get_font(), &book) == 0);
		if (!book_opened) {
			app_sd_deinit();
		}
	}

	for (size_t i = 0; i < raw_count && entry_count < APP_BOOKMARKS_MAX_ENTRIES; i++) {
		char label[sizeof(raw[i].label) + 1];
		char chapter_title[APP_BOOKMARKS_CHAPTER_TITLE_LEN + 1];
		uint32_t row_index;
		uint32_t page_number = 0;
		bool have_chapter = false;
		const char *prefix = NULL;

		memcpy(label, raw[i].label, sizeof(raw[i].label));
		label[sizeof(raw[i].label)] = '\0';

		if (book_opened) {
			if (content_rows_per_page > 0 &&
			    npb_find_row(&book, raw[i].text_offset, &row_index) == 0) {
				page_number = row_index / content_rows_per_page + 1;
			}

			have_chapter = chapter_title_for_offset(&book, raw[i].text_offset,
								 chapter_title,
								 sizeof(chapter_title));
		}

		if (label[0] != '\0') {
			prefix = label;
		} else if (have_chapter) {
			prefix = chapter_title;
		}

		if (prefix && page_number != 0) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "%s - Page %u", prefix, page_number);
		} else if (prefix) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "%s", prefix);
		} else if (page_number != 0) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "Page %u", page_number);
		} else {
			strncpy(entries[entry_count].text, "Bookmark",
				sizeof(entries[entry_count].text) - 1);
			entries[entry_count].text[sizeof(entries[entry_count].text) - 1] = '\0';
		}

		entries[entry_count].text_offset = raw[i].text_offset;
		entries[entry_count].is_back = false;
		entry_count++;
	}

	if (book_opened) {
		npb_close(&book);
		app_sd_deinit();
	}

	selected_index = 0;
	scroll_offset = 0;

	return 0;
}

static void draw_name(uint16_t row, const char *name)
{
	size_t len = strlen(name);

	for (uint16_t col = APP_BOOKMARKS_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_BOOKMARKS_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? name[i] : ' ');
	}
}

static void draw_entry(uint16_t page_row, size_t idx)
{
	uint16_t row = content_start_row + page_row;

	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	draw_name(row, entries[idx].text);
}

static void draw_cursor(uint16_t page_row, bool show)
{
	uint16_t row = content_start_row + page_row;

	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_BOOKMARKS_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_BOOKMARKS_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_BOOKMARKS_CURSOR_COL + 1, ' ');
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

static void bookmarks_flush(void)
{
	partial_count++;

	if (partial_count >= APP_BOOKMARKS_FULL_REFRESH_EVERY) {
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

	bookmarks_flush();
}

static void select_entry(void)
{
	bool is_back;
	void (*cb)(void);

	if (entry_count == 0) {
		return;
	}

	is_back = entries[selected_index].is_back;

	if (!is_back) {
		int ret = app_reader_jump_to_offset(entries[selected_index].text_offset);

		if (ret) {
			printk("bookmarks: failed to jump to bookmark (%d)\n", ret);
		}
	}

	cb = is_back ? on_back_cb : on_selected_cb;

	bookmarks_active = false;
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

void app_bookmarks_show(void (*on_back)(void), void (*on_selected)(void))
{
	uint16_t rows;

	on_back_cb = on_back;
	on_selected_cb = on_selected;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	load_bookmarks();

	redraw_page();
	app_header_update("Bookmarks", false);

	app_text_full_refresh();
	partial_count = 0;

	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(NULL);
	bookmarks_active = true;
}

bool app_bookmarks_is_active(void)
{
	return bookmarks_active;
}
