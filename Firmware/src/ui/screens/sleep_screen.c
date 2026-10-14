/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Screen left on the e-paper while the device sleeps: the NanoPage logo in the
 * top row, today's date centred below it, then the calendar day view. The status
 * header is not shown.
 *
 * Called from the power module right before System OFF. The calendar store is
 * read from the SD card here (it mounts and powers the card itself); if the
 * load fails the snapshot just shows an empty day.
 */

#include <string.h>

#include "calendar_day.h"
#include "logo_nanopage.h"
#include "sleep_screen.h"
#include "text.h"

#define SLEEP_LOGO_ROW 0
#define SLEEP_DATE_ROW 1

int app_sleep_screen_show(void)
{
	char date[24];
	uint16_t cols;
	uint16_t rows;
	int ret;

	/* Also clears the framebuffer, whatever screen was up before. */
	ret = app_text_set_font(APP_TEXT_FONT_TAMZEN);
	if (ret) {
		return ret;
	}

	app_calendar_day_draw_snapshot();

	app_text_draw_bitmap(SLEEP_LOGO_ROW,
			     (uint16_t)((app_text_width_px() - APP_LOGO_NANOPAGE_WIDTH) / 2),
			     APP_LOGO_NANOPAGE_WIDTH, app_logo_nanopage);

	/* The snapshot just read today's date and the day offset is 0, so this is today. */
	if (app_calendar_day_format_date(date, sizeof(date)) == 0) {
		size_t len = strlen(date);
		uint16_t first_col;

		app_text_get_grid(&cols, &rows);
		first_col = (cols > len) ? (uint16_t)((cols - len) / 2) : 0;
		for (size_t i = 0; i < len; i++) {
			app_text_draw_cell(SLEEP_DATE_ROW, (uint16_t)(first_col + i), date[i]);
		}
	}

	return app_text_full_refresh();
}
