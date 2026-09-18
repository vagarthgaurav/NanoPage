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

/* Row layout - a plain cursor + name list, no per-entry type icon (unlike
 * browser.c/menu.c's folder/file or app icons): every entry here is a
 * menu action, not a typed object, so a second icon column wouldn't add
 * information - the freed columns go to name width instead.
 */
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

/* Set once per app_reader_menu_show() call, handed straight through to
 * toc.c unchanged (see reader_menu.h's top comment - there's only ever
 * one exit path back to reading, regardless of how many menu levels deep
 * the user went).
 */
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

/* Three entries always fit on one page (same reasoning as menu.c's own
 * redraw()) - no scrolling logic needed here, unlike toc.c's.
 */
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

/*
 * toc.c/bookmarks.c's "Back" hook (their own on_back, not this menu's
 * own launch_back() above) - redisplays this same menu, with the same
 * exit_cb it already has, rather than jumping straight back to reading.
 * That's the whole point of this indirection: without it, toc.c/
 * bookmarks.c's "Back" would have nothing to return to but exit_cb
 * itself (reader.c's reopen_reading()), skipping this menu entirely -
 * which is what the very first version of this screen did, before
 * anyone noticed it meant "Back" inside the chapter list or bookmark
 * list dumped you straight into the book instead of back to the menu
 * you came from.
 */
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

	/* Always a full refresh - opening this menu is an infrequent,
	 * deliberate transition (same reasoning as menu.c's own
	 * app_menu_show()), not an ordinary page turn.
	 */
	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	menu_active = true;
}

bool app_reader_menu_is_active(void)
{
	return menu_active;
}
