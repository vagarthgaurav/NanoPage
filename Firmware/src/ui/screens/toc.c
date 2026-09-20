/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "config.h"
#include "header.h"
#include "list_widget.h"
#include "npb.h"
#include "reader.h"
#include "sd.h"
#include "text.h"
#include "toc.h"

#define APP_TOC_CURSOR_COL 0
#define APP_TOC_NAME_COL   3

#define APP_TOC_MAX_CHAPTERS 63
#define APP_TOC_MAX_ENTRIES  (APP_TOC_MAX_CHAPTERS + 1)

#define APP_TOC_TITLE_LEN 64

struct toc_entry {
	char title[APP_TOC_TITLE_LEN + 1];
	uint32_t text_offset;
	bool is_back;
};

static struct toc_entry entries[APP_TOC_MAX_ENTRIES];
static size_t entry_count;

static struct app_list_widget list;

static void (*on_back_cb)(void);
static void (*on_selected_cb)(void);

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;
static bool toc_active;

static void on_button(enum app_button button);
static void draw_entry(uint16_t row, size_t idx, void *user_data);

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

static void draw_entry(uint16_t row, size_t idx, void *user_data)
{
	ARG_UNUSED(user_data);

	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	draw_name(row, entries[idx].title);
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
	app_list_widget_move_cursor(&list, delta);
	toc_flush();
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
			printk("toc: failed to jump to chapter (%d)\n", ret);
		}
	}

	cb = is_back ? on_back_cb : on_selected_cb;

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

void app_toc_show(void (*on_back)(void), void (*on_selected)(void))
{
	uint16_t rows;

	on_back_cb = on_back;
	on_selected_cb = on_selected;

	app_text_get_grid(&grid_cols, &rows);
	app_text_get_content_rows(&content_start_row, &content_rows);

	app_list_widget_init(&list, draw_entry, NULL, APP_TOC_CURSOR_COL, content_start_row,
			      content_rows);

	load_chapters();
	app_list_widget_reset(&list, entry_count);

	app_list_widget_redraw(&list);
	app_header_update("Table of Contents", false);

	app_text_full_refresh();
	partial_count = 0;

	app_buttons_set_press_callback(on_button);
	app_buttons_set_long_press_callback(NULL);
	toc_active = true;
}

bool app_toc_is_active(void)
{
	return toc_active;
}
