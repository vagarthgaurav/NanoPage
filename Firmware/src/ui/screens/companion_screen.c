/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "companion.h"
#include "companion_screen.h"
#include "header.h"
#include "text.h"

#define APP_COMPANION_PASSKEY_FONT APP_TEXT_FONT_TAMZEN_LARGE
#define APP_COMPANION_PASSKEY_FONT_WIDTH 10

static void (*exit_cb)(void);
static bool screen_active;
static bool bt_started;

/* Draws the lines centered in the content area and returns the first row used. */
static uint16_t draw_message(const char *const *lines, size_t line_count)
{
	uint16_t grid_cols, rows, content_start_row, content_rows, start_row;

	app_text_get_grid(&grid_cols, &rows);
	app_text_get_content_rows(&content_start_row, &content_rows);

	start_row = (uint16_t)(content_start_row +
			       ((content_rows > line_count) ? (content_rows - line_count) / 2 : 0));

	for (uint16_t i = 0; i < content_rows; i++) {
		app_text_draw_row((uint16_t)(content_start_row + i), "");
	}

	for (size_t i = 0; i < line_count && (start_row - content_start_row + i) < content_rows;
	     i++) {
		char padded[64];
		size_t len = strlen(lines[i]);
		size_t pad = (len < grid_cols) ? (size_t)(grid_cols - len) / 2 : 0;

		if (pad + len >= sizeof(padded)) {
			pad = 0;
		}

		memset(padded, ' ', pad);
		strncpy(padded + pad, lines[i], sizeof(padded) - pad - 1);
		padded[sizeof(padded) - 1] = '\0';

		app_text_draw_row((uint16_t)(start_row + i), padded);
	}

	return start_row;
}

static void draw_advertising(void)
{
	static const char *const lines[] = {
		"Connect to phone",
		"",
		"Open the NanoPage app",
		"and pick NanoPage",
		"",
		"Action: cancel",
	};

	draw_message(lines, ARRAY_SIZE(lines));
}

static void draw_passkey(uint32_t passkey)
{
	static const char *const lines[] = {
		"Pairing",
		"",
		"", /* passkey, drawn in a larger font below */
		"",
		"Enter this on your phone",
	};
	char digits[12];
	char padded[32];
	size_t pad, len;
	uint16_t start_row = draw_message(lines, ARRAY_SIZE(lines));

	passkey %= 1000000;
	snprintf(digits, sizeof(digits), "%03u %03u", (unsigned int)(passkey / 1000),
		 (unsigned int)(passkey % 1000));

	len = strlen(digits);
	pad = (app_text_width_px() / APP_COMPANION_PASSKEY_FONT_WIDTH - len) / 2;
	memset(padded, ' ', pad);
	strcpy(padded + pad, digits);

	/* Drawn after the rows so it wins over the blank row it may spill into. */
	app_text_draw_text_px((uint16_t)((start_row + 2) * app_text_row_height()), padded,
			      APP_COMPANION_PASSKEY_FONT);
}

static void draw_connected(void)
{
	static const char *const lines[] = {
		"Connected",
		"",
		"Syncing happens in the app",
		"",
		"Action: disconnect",
	};

	draw_message(lines, ARRAY_SIZE(lines));
}

static void draw_unavailable(void)
{
	static const char *const lines[] = {
		"Bluetooth unavailable",
		"",
		"Action: back",
	};

	draw_message(lines, ARRAY_SIZE(lines));
}

static void leave(void)
{
	void (*cb)(void) = exit_cb;

	exit_cb = NULL;
	screen_active = false;
	bt_started = false;
	app_buttons_set_press_callback(NULL);

	if (cb) {
		cb();
	}
}

static void on_state(enum app_companion_state state, uint32_t passkey)
{
	if (!screen_active) {
		return;
	}

	switch (state) {
	case APP_COMPANION_STATE_ADVERTISING:
		draw_advertising();
		break;
	case APP_COMPANION_STATE_PASSKEY:
		draw_passkey(passkey);
		break;
	case APP_COMPANION_STATE_CONNECTED:
		/* The link stays up in the background so the other apps stay
		 * usable; reopening this screen offers the disconnect.
		 */
		leave();
		return;
	case APP_COMPANION_STATE_TIMED_OUT:
	case APP_COMPANION_STATE_OFF:
		printk("companion_screen: session ended (%d)\n", state);
		leave();
		return;
	}

	app_text_full_refresh();
}

static void on_button(enum app_button button)
{
	if (button != APP_BUTTON_ACTION || !screen_active) {
		return;
	}

	if (bt_started) {
		app_companion_stop();
	}

	leave();
}

void app_companion_screen_show(void (*on_exit)(void))
{
	int ret;

	exit_cb = on_exit;
	screen_active = true;

	app_header_update("Phone", false);
	draw_advertising();
	app_text_full_refresh();
	app_buttons_set_press_callback(on_button);

	ret = app_companion_start(on_state);
	if (ret == -EALREADY) {
		/* Session left running in the background; pick it back up. */
		if (app_companion_get_state() == APP_COMPANION_STATE_CONNECTED) {
			draw_connected();
			app_text_full_refresh();
		}
		bt_started = true;
		return;
	}
	if (ret) {
		printk("companion_screen: start failed (%d)\n", ret);
		draw_unavailable();
		app_text_full_refresh();
		return;
	}

	bt_started = true;
}

bool app_companion_screen_is_active(void)
{
	return screen_active;
}
