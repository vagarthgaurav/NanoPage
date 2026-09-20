/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "browser.h"
#include "buttons.h"
#include "font_siji.h"
#include "header.h"
#include "list_widget.h"
#include "menu.h"
#include "settings.h"
#include "text.h"

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

	if (idx >= APP_MENU_ENTRY_COUNT) {
		return;
	}

	len = strlen(entries[idx].name);

	app_text_draw_icon(row, app_text_col_to_px(APP_MENU_ICON_COL), entries[idx].icon);

	for (uint16_t col = APP_MENU_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_MENU_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? entries[idx].name[i] : ' ');
	}
}

static void move_cursor(int delta)
{
	app_list_widget_move_cursor(&list, delta);
	app_text_flush();
}

static void launch_browser(void)
{
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
		entries[app_list_widget_selected_index(&list)].launch();
		break;
	}
}

void app_menu_show(void)
{
	uint16_t rows;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	app_list_widget_init(&list, draw_entry, NULL, APP_MENU_CURSOR_COL, content_start_row,
			      content_rows);
	app_list_widget_reset(&list, APP_MENU_ENTRY_COUNT);
	app_list_widget_redraw(&list);
	app_header_update("Apps", false);

	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	menu_active = true;
}

bool app_menu_is_active(void)
{
	return menu_active;
}
