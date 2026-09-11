/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/kernel.h>

#include "display.h"
#include "fuel_gauge.h"
#include "max17055.h"
#include "rtc.h"
#include "text.h"

/*
 * Status-bar refresh cadence: partial refresh is cheap (text.c only
 * pushes the one changed row), but even a sub-second SSD1680 "fast"
 * waveform update isn't free against the battery-life goal for a value
 * (SOC/voltage) that barely moves second to second. 30s is a placeholder
 * - revisit once there's real battery-life data to tune against.
 */
#define STATUS_REFRESH_INTERVAL K_SECONDS(30)

static uint16_t status_row;

/* Draws into text.c's framebuffer only - caller pushes it (app_text_flush()
 * for a cheap partial update, or app_text_full_refresh() for the initial
 * ghost-free draw).
 */
static void draw_status(void)
{
	struct max17055_data_snapshot snap;
	char buf[48];

	if (app_fuel_gauge_read(&snap)) {
		return;
	}

	snprintf(buf, sizeof(buf), "%u.%u%%  %d.%03dV  %dmA", snap.soc_x10 / 10,
		 snap.soc_x10 % 10, (int)(snap.voltage_uv / 1000000),
		 (int)((snap.voltage_uv / 1000) % 1000), (int)(snap.current_ua / 1000));

	app_text_draw_row(status_row, buf);
}

int main(void)
{
	printk("NanoPage: RTC + buttons + display + fuel gauge bring-up.\n");

	if (app_rtc_init()) {
		return 0;
	}

	app_display_init();
	app_fuel_gauge_init();

	if (app_text_init(APP_TEXT_FONT_SMALL) == 0) {
		uint16_t cols;

		app_text_get_grid(&cols, &status_row);
		status_row -= 1;

		draw_status();
		app_text_full_refresh();
	}

	while (1) {
		k_sleep(STATUS_REFRESH_INTERVAL);

		draw_status();
		app_text_flush();

		if (app_fuel_gauge_should_save_learned_params()) {
			app_fuel_gauge_save_learned_params();
		}
	}

	return 0;
}
