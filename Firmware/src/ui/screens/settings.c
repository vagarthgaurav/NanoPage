/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "header.h"
#include "reader.h"
#include "settings.h"
#include "text.h"

struct text_size_option {
	const char *label;
	enum app_text_font font;
};

static const struct text_size_option text_sizes[] = {
	{"Small", APP_TEXT_FONT_TAMZEN},
	{"Medium", APP_TEXT_FONT_TAMZEN_MEDIUM},
	{"Large", APP_TEXT_FONT_TAMZEN_LARGE},
};

#define APP_SETTINGS_TEXT_SIZE_COUNT ARRAY_SIZE(text_sizes)

static void (*exit_cb)(void);
static uint16_t content_start_row;
static size_t text_size_index;
static bool settings_active;

static size_t font_to_index(enum app_text_font font)
{
	for (size_t i = 0; i < APP_SETTINGS_TEXT_SIZE_COUNT; i++) {
		if (text_sizes[i].font == font) {
			return i;
		}
	}

	return 0; /* shouldn't happen - reader_font is always one of these */
}

static void draw(void)
{
	char line[40];

	snprintf(line, sizeof(line), "Book text size: %s", text_sizes[text_size_index].label);
	app_text_draw_row(content_start_row, line);
	app_text_draw_row((uint16_t)(content_start_row + 1), "");
	app_text_draw_row((uint16_t)(content_start_row + 2), "Prev/Next: change");
	app_text_draw_row((uint16_t)(content_start_row + 3), "Action: back");
}

static void change_text_size(int delta)
{
	if (delta < 0) {
		if (text_size_index == 0) {
			return;
		}
		text_size_index--;
	} else {
		if (text_size_index + 1 >= APP_SETTINGS_TEXT_SIZE_COUNT) {
			return;
		}
		text_size_index++;
	}

	app_reader_set_font(text_sizes[text_size_index].font);

	draw();
	app_text_flush();
}

static void leave(void)
{
	void (*cb)(void) = exit_cb;

	exit_cb = NULL;
	settings_active = false;
	app_buttons_set_press_callback(NULL);

	if (cb) {
		cb();
	}
}

static void on_button(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		change_text_size(-1);
		break;
	case APP_BUTTON_NEXT:
		change_text_size(1);
		break;
	case APP_BUTTON_ACTION:
		leave();
		break;
	}
}

void app_settings_init(void (*on_exit)(void))
{
	uint16_t content_rows;

	exit_cb = on_exit;
	text_size_index = font_to_index(app_reader_get_font());

	app_text_get_content_rows(&content_start_row, &content_rows);

	for (uint16_t row = content_start_row; row < content_start_row + content_rows; row++) {
		app_text_draw_row(row, "");
	}

	draw();
	app_header_update("Settings", false);
	app_text_full_refresh();

	app_buttons_set_press_callback(on_button);
	settings_active = true;
}

bool app_settings_is_active(void)
{
	return settings_active;
}
