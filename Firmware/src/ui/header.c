/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>

#include "charger.h"
#include "fuel_gauge.h"
#include "font_siji.h"
#include "header.h"
#include "max17055.h"
#include "rtc.h"
#include "text.h"

/*
 * Right-cluster layout (charging icon -> percentage -> battery icon,
 * right-aligned to the screen edge) is built in column units even though
 * the icons are pixel-positioned: font_siji.h's icons are 12px wide, which
 * happens to be an exact multiple of Tamzen's 6px cell width (2 columns),
 * so laying everything out in columns and converting to pixels only at the
 * two app_text_draw_icon() calls keeps icon and text edges aligned without
 * any fractional-pixel bookkeeping. 1 column of gap is left between each
 * element (icon/percent/icon) purely for visual breathing room.
 */
#define APP_HEADER_ELEMENT_GAP_COLS 1
#define APP_HEADER_ICON_COLS        2 /* 12px icon / 6px cell width */

static void draw_str(uint16_t row, uint16_t col, const char *str, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		app_text_draw_cell(row, (uint16_t)(col + i), str[i]);
	}
}

/* Last-drawn state, for app_header_update()'s change check. have_last is
 * false until the first successful draw, so that first call always redraws
 * regardless of what these happen to be initialized to.
 */
static bool have_last;
static char last_date_buf[24];
static uint8_t last_percent;
static bool last_charging;

int app_header_update(const char *folder)
{
	struct rtc_time now;
	struct max17055_data_snapshot snap;
	uint16_t cols, rows;
	/* Real output is always exactly 14 chars + NUL ("dd/mm/yy HH:mm") -
	 * oversized so GCC's format-truncation check (which assumes each
	 * %02d field's full int range, not struct rtc_time's documented
	 * bounds) doesn't warn.
	 */
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

	/* Each field masked to % 100 so GCC's format-truncation check can see
	 * a 2-digit bound from the value itself, not just int's full range -
	 * struct rtc_time's documented field ranges already guarantee this
	 * (tm_mday <= 31, tm_mon <= 11, tm_hour <= 23, tm_min <= 59), the mask
	 * is only to make that visible to the compiler.
	 */
	snprintf(date_buf, sizeof(date_buf), "%02d/%02d/%02d %02d:%02d", now.tm_mday % 100,
		 (now.tm_mon + 1) % 100, (now.tm_year + 1900) % 100, now.tm_hour % 100,
		 now.tm_min % 100);
	date_len = strlen(date_buf);

	ret = app_fuel_gauge_read(&snap);
	if (ret) {
		return ret;
	}

	percent = (uint8_t)(snap.soc_x10 / 10);
	charging = app_charger_is_charging();

	/* strcmp instead of comparing individual date/time fields - simpler,
	 * and the formatted string is exactly what's visible, so a mismatch
	 * here means something the user would actually see changed.
	 */
	if (have_last && strcmp(date_buf, last_date_buf) == 0 && percent == last_percent &&
	    charging == last_charging) {
		return 0;
	}

	have_last = true;
	memcpy(last_date_buf, date_buf, date_len + 1);
	last_percent = percent;
	last_charging = charging;

	app_text_get_grid(&cols, &rows);

	/* Blank the whole row first - every element below only draws its own
	 * cells, so anything from a previous draw (e.g. a longer folder name)
	 * would otherwise linger.
	 */
	app_text_draw_row(APP_HEADER_ROW, "");

	date_col = (cols > date_len) ? (int)(cols - date_len) / 2 : 0;

	folder_len = strlen(folder);
	folder_max = (date_col > APP_HEADER_ELEMENT_GAP_COLS) ? (size_t)date_col - 1 : 0;
	if (folder_len > folder_max) {
		folder_len = folder_max;
	}
	draw_str(APP_HEADER_ROW, 0, folder, folder_len);

	draw_str(APP_HEADER_ROW, (uint16_t)date_col, date_buf, date_len);

	battery_col = (cols >= APP_HEADER_ICON_COLS) ? (int)cols - APP_HEADER_ICON_COLS : 0;

	snprintf(percent_buf, sizeof(percent_buf), "%u%%", percent);
	percent_len = strlen(percent_buf);
	percent_col = battery_col - APP_HEADER_ELEMENT_GAP_COLS - (int)percent_len;
	if (percent_col < 0) {
		percent_col = 0;
	}
	draw_str(APP_HEADER_ROW, (uint16_t)percent_col, percent_buf, percent_len);

	/* BQ25170 STAT (charger.c) - the charger's own status line, not the
	 * fuel gauge's measured current direction, so the icon stays correct
	 * even once charge current tapers to ~0 near full (current-sign would
	 * drop out there even though USB is still connected).
	 */
	if (charging) {
		charging_col = percent_col - APP_HEADER_ELEMENT_GAP_COLS - APP_HEADER_ICON_COLS;
		if (charging_col >= 0) {
			app_text_draw_icon(APP_HEADER_ROW,
					    app_text_col_to_px((uint16_t)charging_col),
					    APP_TEXT_ICON_CHARGING);
		}
	}

	/* Round to the nearest of the 9 fill levels rather than always
	 * flooring, so e.g. 95% shows a full icon instead of one level down.
	 */
	level = (uint8_t)((percent * (APP_TEXT_ICON_BATTERY_LEVELS - 1) + 50) / 100);
	app_text_draw_icon(APP_HEADER_ROW, app_text_col_to_px((uint16_t)battery_col),
			    (enum app_text_icon)(APP_TEXT_ICON_BATTERY_MIN + level));

	/* Separator rule along the row's bottom pixel line - shares row 0's
	 * dirty tracking/flush instead of needing a grid row of its own.
	 */
	app_text_draw_hline(APP_HEADER_ROW, app_text_row_height() - 1, true);

	return 1;
}
