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
#define STATUS_REFRESH_INTERVAL_SEC 30

/*
 * How long before the refresh to sample the instantaneous Current
 * register. Unlike AvgCurrent (chip-filtered, immune to this), a raw
 * Current sample taken right at refresh time would land on top of the
 * SSD1680 partial-refresh SPI burst that follows it a moment later,
 * reading elevated/noisy current rather than true idle draw. Sampling
 * 5s ahead of the refresh, with nothing else going on in between, gives
 * a clean instantaneous idle sample instead.
 */
#define STATUS_CURRENT_SAMPLE_LEAD_SEC 5

static uint16_t status_row;
static int32_t last_instant_current_ua;

/* Samples the raw Current register only, STATUS_CURRENT_SAMPLE_LEAD
 * ahead of the next draw_status(), while the board is otherwise idle.
 * Leaves last_instant_current_ua unchanged on I2C failure.
 */
static void sample_instant_current(void)
{
	struct max17055_data_snapshot snap;

	if (app_fuel_gauge_read(&snap) == 0) {
		last_instant_current_ua = snap.current_ua;
	}
}

/* Formats a signed microamp current as "-12.345mA" (3 decimal places -
 * the ADC's own LSB is 156.25uA for this board's 10mOhm sense resistor,
 * see reg_to_current_ua() in max17055.c, so whole-mA truncation would
 * throw away the only resolution the chip actually has to offer).
 */
static void format_current_ma(char *out, size_t len, int32_t current_ua)
{
	bool neg = current_ua < 0;

	if (neg) {
		current_ua = -current_ua;
	}

	snprintf(out, len, "%s%d.%03dmA", neg ? "-" : "", (int)(current_ua / 1000),
		 (int)(current_ua % 1000));
}

/* Draws into text.c's framebuffer only - caller pushes it (app_text_flush()
 * for a cheap partial update, or app_text_full_refresh() for the initial
 * ghost-free draw).
 */
static void draw_status(void)
{
	struct max17055_data_snapshot snap;
	char buf[72];
	char inst_buf[16];
	char avg_buf[16];

	if (app_fuel_gauge_read(&snap)) {
		return;
	}

	format_current_ma(inst_buf, sizeof(inst_buf), last_instant_current_ua);
	format_current_ma(avg_buf, sizeof(avg_buf), snap.avg_current_ua);

	snprintf(buf, sizeof(buf), "%u.%u%%  %d.%03dV  I%s A%s", snap.soc_x10 / 10,
		 snap.soc_x10 % 10, (int)(snap.voltage_uv / 1000000),
		 (int)((snap.voltage_uv / 1000) % 1000), inst_buf, avg_buf);

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

	if (app_text_init(APP_TEXT_FONT_TAMZEN) == 0) {
		uint16_t cols;

		app_text_get_grid(&cols, &status_row);
		status_row -= 1;

		app_power_show_wake_message();

		sample_instant_current();
		draw_status();
		app_text_full_refresh();
	}

	while (1) {
		k_sleep(K_SECONDS(STATUS_REFRESH_INTERVAL_SEC - STATUS_CURRENT_SAMPLE_LEAD_SEC));

		sample_instant_current();

		k_sleep(K_SECONDS(STATUS_CURRENT_SAMPLE_LEAD_SEC));

		draw_status();
		app_text_flush();

		if (app_fuel_gauge_should_save_learned_params()) {
			app_fuel_gauge_save_learned_params();
		}

		app_power_check_idle();
	}

	return 0;
}
