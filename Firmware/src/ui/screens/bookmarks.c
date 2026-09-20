/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "bookmarks.h"
#include "buttons.h"
#include "config.h"
#include "header.h"
#include "list_widget.h"
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

static struct app_list_widget list;

static void (*on_back_cb)(void);
static void (*on_selected_cb)(void);

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;
static bool bookmarks_active;

static void on_button(enum app_button button);
static void draw_entry(uint16_t row, size_t idx, void *user_data);

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

static void draw_entry(uint16_t row, size_t idx, void *user_data)
{
	ARG_UNUSED(user_data);

	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	draw_name(row, entries[idx].text);
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
	app_list_widget_move_cursor(&list, delta);
	bookmarks_flush();
}

static void select_entry(void)
{
	size_t idx;
	bool is_back;
	void (*cb)(void);

	if (entry_count == 0) {
		return;
	}

	idx = app_list_widget_selected_index(&list);
	is_back = entries[idx].is_back;

	if (!is_back) {
		int ret = app_reader_jump_to_offset(entries[idx].text_offset);

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

	app_list_widget_init(&list, draw_entry, NULL, APP_BOOKMARKS_CURSOR_COL, content_start_row,
			      content_rows);

	load_bookmarks();
	app_list_widget_reset(&list, entry_count);

	app_list_widget_redraw(&list);
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
