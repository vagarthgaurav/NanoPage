/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/kernel.h>

#include "display.h"
#include "fuel_gauge.h"
#include "max17055.h"
#include "power.h"
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
	int32_t current_ua;
	bool current_neg;

	if (app_fuel_gauge_read(&snap)) {
		return;
	}

	/*
	 * AvgCurrent, not the raw Current register: a raw read is an
	 * instantaneous sample of whatever the MAX17055's free-running ADC
	 * last captured, and that's liable to land right on the CPU's own
	 * I2C-burst-driven wake-up to do this very read (~3mA+ active vs.
	 * ~40uA true idle) rather than the ~30s of real sleep in between.
	 * AvgCurrent is filtered on-chip over a much longer window, so it
	 * isn't skewed by our own polling.
	 *
	 * Printed to 3 decimal places (uA resolution) rather than truncated
	 * whole mA - the ADC's own LSB is 156.25uA for this board's 10mOhm
	 * sense resistor (see reg_to_current_ua() in max17055.c), so
	 * whole-mA truncation was throwing away the only resolution the
	 * chip actually has to offer.
	 */
	current_ua = snap.avg_current_ua;
	current_neg = current_ua < 0;
	if (current_neg) {
		current_ua = -current_ua;
	}

	snprintf(buf, sizeof(buf), "%u.%u%%  %d.%03dV  %s%d.%03dmA", snap.soc_x10 / 10,
		 snap.soc_x10 % 10, (int)(snap.voltage_uv / 1000000),
		 (int)((snap.voltage_uv / 1000) % 1000), current_neg ? "-" : "",
		 (int)(current_ua / 1000), (int)(current_ua % 1000));

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
	app_power_init();

	if (app_text_init(APP_TEXT_FONT_SMALL) == 0) {
		uint16_t cols;

		app_text_get_grid(&cols, &status_row);
		status_row -= 1;

		app_power_show_wake_message();

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

		app_power_check_idle();
	}

	return 0;
}
