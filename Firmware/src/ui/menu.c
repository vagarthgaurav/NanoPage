/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "browser.h"
#include "buttons.h"
#include "font_siji.h"
#include "header.h"
#include "menu.h"
#include "settings.h"
#include "text.h"

/* Row layout, in text-grid columns - same convention as browser.c's listing
 * (a menu entry and a directory entry are both "an icon plus a name"):
 *   col 0-1: cursor icon (blank unless this is the selected row)
 *   col 2-3: app icon
 *   col 4:   gap
 *   col 5..: app name
 */
#define APP_MENU_CURSOR_COL    0
#define APP_MENU_ICON_COL      2
#define APP_MENU_NAME_COL      5

static void launch_browser(void);
static void launch_settings(void);

struct menu_entry {
	const char *name;
	enum app_text_icon icon;
	void (*launch)(void);
};

static const struct menu_entry entries[] = {
	{"File Browser", APP_TEXT_ICON_FOLDER, launch_browser},
	{"Settings", APP_TEXT_ICON_SETTINGS, launch_settings},
};

#define APP_MENU_ENTRY_COUNT ARRAY_SIZE(entries)

static size_t selected_index;
static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;
static bool menu_active;

static void on_button(enum app_button button);

static void draw_entry(uint16_t row, size_t idx)
{
	size_t len = strlen(entries[idx].name);

	app_text_draw_icon(row, app_text_col_to_px(APP_MENU_ICON_COL), entries[idx].icon);

	for (uint16_t col = APP_MENU_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_MENU_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? entries[idx].name[i] : ' ');
	}
}

static void draw_cursor(uint16_t row, bool show)
{
	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_MENU_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_MENU_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_MENU_CURSOR_COL + 1, ' ');
	}
}

/* Every entry fits on one page (only two exist, well under any font's
 * content_rows) - no scrolling logic needed, unlike browser.c's listing.
 */
static void redraw(void)
{
	for (uint16_t row = content_start_row; row < content_start_row + content_rows; row++) {
		app_text_draw_row(row, "");
	}

	for (size_t i = 0; i < APP_MENU_ENTRY_COUNT && i < content_rows; i++) {
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
		if (selected_index + 1 >= APP_MENU_ENTRY_COUNT) {
			return;
		}
		new_index = selected_index + 1;
	}

	draw_cursor((uint16_t)(content_start_row + selected_index), false);
	selected_index = new_index;
	draw_cursor((uint16_t)(content_start_row + selected_index), true);

	app_text_flush();
}

static void launch_browser(void)
{
	/* app_menu_show() is passed as the browser's on_exit - selecting the
	 * SD root's synthetic ".." entry (browser.h) comes straight back
	 * here. On failure (no SD card), the browser never registered its
	 * own button callback (see browser.c), so ours is still live -
	 * nothing to undo, just stay on the menu.
	 */
	if (app_browser_init("/", app_menu_show) == 0) {
		menu_active = false;
	} else {
		printk("menu: file browser unavailable (no SD card?)\n");
	}
}

static void launch_settings(void)
{
	app_settings_init(app_menu_show);
	menu_active = false;
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

void app_menu_show(void)
{
	uint16_t rows;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	redraw();
	app_header_update("Apps");

	/* Always a full refresh - this only ever runs at boot or when
	 * returning from another app (an infrequent, deliberate transition,
	 * same reasoning as browser.c's folder-navigation full refresh), so
	 * there's no partial-refresh counter here to keep.
	 */
	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	menu_active = true;
}

bool app_menu_is_active(void)
{
	return menu_active;
}
