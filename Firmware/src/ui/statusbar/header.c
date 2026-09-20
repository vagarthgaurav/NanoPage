/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>

#include "charger.h"
#include "fuel_gauge.h"
#include "font_siji.h"
#include "header.h"
#include "rtc.h"
#include "text.h"

#define APP_HEADER_ELEMENT_GAP_COLS 1
#define APP_HEADER_ICON_COLS        2 /* 12px icon / 6px cell width */

static void draw_str(uint16_t row, uint16_t col, const char *str, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		app_text_draw_cell(row, (uint16_t)(col + i), str[i]);
	}
}

static bool have_last;
static char last_date_buf[24];
static uint8_t last_percent;
static bool last_charging;
static bool last_bookmarked;
static char last_folder[128];

int app_header_update(const char *folder, bool bookmarked)
{
	struct rtc_time now;
	uint16_t soc_x10;
	uint16_t cols, rows;
	char date_buf[24];
	char percent_buf[8];
	size_t date_len, percent_len, folder_len, folder_max;
	int date_col, battery_col, percent_col, charging_col;
	uint8_t percent, level;
	bool charging;
	int ret;

	ret = app_rtc_get_time(&now);
	if (ret) {
		return ret;
	}

	snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%02d %02d:%02d", now.tm_mday % 100,
		 (now.tm_mon + 1) % 100, (now.tm_year + 1900) % 100, now.tm_hour % 100,
		 now.tm_min % 100);
	date_len = strlen(date_buf);

	ret = app_fuel_gauge_read_percent_x10(&soc_x10);
	if (ret) {
		return ret;
	}

	percent = (uint8_t)(soc_x10 / 10);
	charging = app_charger_is_charging();

	if (have_last && strcmp(date_buf, last_date_buf) == 0 && percent == last_percent &&
	    charging == last_charging && bookmarked == last_bookmarked &&
	    strncmp(folder, last_folder, sizeof(last_folder)) == 0) {
		return 0;
	}

	have_last = true;
	memcpy(last_date_buf, date_buf, date_len + 1);
	last_percent = percent;
	last_charging = charging;
	last_bookmarked = bookmarked;
	snprintf(last_folder, sizeof(last_folder), "%s", folder);

	app_text_get_grid(&cols, &rows);

	app_text_draw_row(APP_HEADER_ROW, "");

	date_col = (cols > date_len) ? (int)(cols - date_len) / 2 : 0;

	if (bookmarked) {
		app_text_draw_icon(APP_HEADER_ROW, app_text_col_to_px(0), APP_TEXT_ICON_BOOKMARK);
	} else {
		folder_len = strlen(folder);
		folder_max = (date_col > APP_HEADER_ELEMENT_GAP_COLS) ? (size_t)date_col - 1 : 0;
		if (folder_len > folder_max) {
			folder_len = folder_max;
		}
		draw_str(APP_HEADER_ROW, 0, folder, folder_len);
	}

	draw_str(APP_HEADER_ROW, (uint16_t)date_col, date_buf, date_len);

	battery_col = (cols >= APP_HEADER_ICON_COLS) ? (int)cols - APP_HEADER_ICON_COLS : 0;

	snprintf(percent_buf, sizeof(percent_buf), "%u%%", percent);
	percent_len = strlen(percent_buf);
	percent_col = battery_col - APP_HEADER_ELEMENT_GAP_COLS - (int)percent_len;
	if (percent_col < 0) {
		percent_col = 0;
	}
	draw_str(APP_HEADER_ROW, (uint16_t)percent_col, percent_buf, percent_len);

	if (charging) {
		charging_col = percent_col - APP_HEADER_ELEMENT_GAP_COLS - APP_HEADER_ICON_COLS;
		if (charging_col >= 0) {
			app_text_draw_icon(APP_HEADER_ROW,
					    app_text_col_to_px((uint16_t)charging_col),
					    APP_TEXT_ICON_CHARGING);
		}
	}

	level = (uint8_t)((percent * (APP_TEXT_ICON_BATTERY_LEVELS - 1) + 50) / 100);
	app_text_draw_icon(APP_HEADER_ROW, app_text_col_to_px((uint16_t)battery_col),
			    (enum app_text_icon)(APP_TEXT_ICON_BATTERY_MIN + level));

	app_text_draw_hline(APP_HEADER_ROW, app_text_row_height() - 1, true);

	return 1;
}
