/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "bookmarks.h"
#include "buttons.h"
#include "font_siji.h"
#include "header.h"
#include "reader_menu.h"
#include "text.h"
#include "toc.h"

#define APP_READER_MENU_CURSOR_COL 0
#define APP_READER_MENU_NAME_COL   3

static void launch_back(void);
static void launch_toc(void);
static void launch_bookmarks(void);

struct reader_menu_entry {
	const char *name;
	void (*launch)(void);
};

static const struct reader_menu_entry entries[] = {
	{"Back to Book", launch_back},
	{"Table of Contents", launch_toc},
	{"Bookmarks", launch_bookmarks},
};

#define APP_READER_MENU_ENTRY_COUNT ARRAY_SIZE(entries)

static void (*exit_cb)(void);

static size_t selected_index;
static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;
static bool menu_active;

static void on_button(enum app_button button);

static void draw_entry(uint16_t row, size_t idx)
{
	size_t len = strlen(entries[idx].name);

	for (uint16_t col = APP_READER_MENU_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_READER_MENU_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? entries[idx].name[i] : ' ');
	}
}

static void draw_cursor(uint16_t row, bool show)
{
	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_READER_MENU_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_READER_MENU_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_READER_MENU_CURSOR_COL + 1, ' ');
	}
}

static void redraw(void)
{
	for (uint16_t row = content_start_row; row < content_start_row + content_rows; row++) {
		app_text_draw_row(row, "");
	}

	for (size_t i = 0; i < APP_READER_MENU_ENTRY_COUNT && i < content_rows; i++) {
		draw_entry((uint16_t)(content_start_row + i), i);
	}

	draw_cursor((uint16_t)(content_start_row + selected_index), true);
}

static void move_cursor(int delta)
{
	size_t new_index;

	if (delta < 0) {
		if (selected_index == 0) {
			return;
		}
		new_index = selected_index - 1;
	} else {
		if (selected_index + 1 >= APP_READER_MENU_ENTRY_COUNT) {
			return;
		}
		new_index = selected_index + 1;
	}

	draw_cursor((uint16_t)(content_start_row + selected_index), false);
	selected_index = new_index;
	draw_cursor((uint16_t)(content_start_row + selected_index), true);

	app_text_flush();
}

static void launch_back(void)
{
	menu_active = false;
	exit_cb();
}

static void return_to_menu(void)
{
	app_reader_menu_show(exit_cb);
}

static void launch_toc(void)
{
	menu_active = false;
	app_toc_show(return_to_menu, exit_cb);
}

static void launch_bookmarks(void)
{
	menu_active = false;
	app_bookmarks_show(return_to_menu, exit_cb);
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
		entries[selected_index].launch();
		break;
	}
}

void app_reader_menu_show(void (*on_exit)(void))
{
	uint16_t rows;

	exit_cb = on_exit;
	selected_index = 0;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	redraw();
	app_header_update("Menu", false);

	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	menu_active = true;
}

bool app_reader_menu_is_active(void)
{
	return menu_active;
}
