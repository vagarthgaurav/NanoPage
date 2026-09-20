/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "bookmarks.h"
#include "buttons.h"
#include "header.h"
#include "list_widget.h"
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

static struct app_list_widget list;

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;
static bool menu_active;

static void on_button(enum app_button button);

static void draw_entry(uint16_t row, size_t idx, void *user_data)
{
	size_t len;

	ARG_UNUSED(user_data);

	app_text_draw_row(row, "");

	if (idx >= APP_READER_MENU_ENTRY_COUNT) {
		return;
	}

	len = strlen(entries[idx].name);

	for (uint16_t col = APP_READER_MENU_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_READER_MENU_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? entries[idx].name[i] : ' ');
	}
}

static void move_cursor(int delta)
{
	app_list_widget_move_cursor(&list, delta);
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
		entries[app_list_widget_selected_index(&list)].launch();
		break;
	}
}

void app_reader_menu_show(void (*on_exit)(void))
{
	uint16_t rows;

	exit_cb = on_exit;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	app_list_widget_init(&list, draw_entry, NULL, APP_READER_MENU_CURSOR_COL, content_start_row,
			      content_rows);
	app_list_widget_reset(&list, APP_READER_MENU_ENTRY_COUNT);
	app_list_widget_redraw(&list);
	app_header_update("Menu", false);

	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	menu_active = true;
}

bool app_reader_menu_is_active(void)
{
	return menu_active;
}
