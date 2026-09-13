/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>

#include "charger.h"
#include "display.h"
#include "footer.h"
#include "fuel_gauge.h"
#include "header.h"
#include "power.h"
#include "rtc.h"
#include "sd.h"
#include "text.h"

/*
 * Footer refresh cadence: partial refresh is cheap (text.c only pushes
 * the one changed row), but even a sub-second SSD1680 "fast" waveform
 * update isn't free against the battery-life goal for a value (SOC/
 * voltage) that barely moves second to second. 30s is a placeholder -
 * revisit once there's real battery-life data to tune against. Unlike
 * the header (event-driven - see on_header_event() below), nothing tells
 * the footer's values changed, so it stays on a plain timer.
 */
#define STATUS_REFRESH_INTERVAL_SEC 30

/*
 * How long before the refresh to sample the instantaneous Current
 * register - see app_footer_sample_current() in footer.h for why this
 * needs to happen ahead of the draw rather than at draw time.
 */
#define STATUS_CURRENT_SAMPLE_LEAD_SEC 5

/* Placeholder until a real file browser/page-location tracker exists -
 * see app_header_update()'s "folder" argument in header.h.
 */
#define HEADER_FOLDER_PLACEHOLDER "/books"

/*
 * The footer's periodic sample-then-draw cycle, as a k_work_delayable
 * chain on the system workqueue instead of a main-thread sleep loop -
 * see the comment in main() for why everything that touches text.c's
 * framebuffer needs to live on one thread.
 */
static struct k_work_delayable footer_sample_work;
static struct k_work_delayable footer_draw_work;

/*
 * Registered with both app_rtc_set_alarm() (fires once a minute) and
 * app_charger_set_change_callback() (fires on every STAT transition) -
 * see header.h for exactly what app_header_update() checks before
 * deciding whether there's actually anything new to draw.
 */
static void on_header_event(void)
{
	app_fuel_gauge_notify_charger_state(app_charger_is_charging());

	if (app_header_update(HEADER_FOLDER_PLACEHOLDER) > 0) {
		app_text_flush();
	}
}

static void footer_draw_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_footer_draw();
	app_text_flush();

	if (app_fuel_gauge_should_save_learned_params()) {
		app_fuel_gauge_save_learned_params();
	}

	app_power_check_idle();

	k_work_schedule(&footer_sample_work,
			K_SECONDS(STATUS_REFRESH_INTERVAL_SEC - STATUS_CURRENT_SAMPLE_LEAD_SEC));
}

static void footer_sample_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_footer_sample_current();
	k_work_schedule(&footer_draw_work, K_SECONDS(STATUS_CURRENT_SAMPLE_LEAD_SEC));
}

int main(void)
{
	printk("NanoPage: RTC + buttons + display + fuel gauge bring-up.\n");

	if (app_rtc_init()) {
		return 0;
	}

	app_display_init();
	app_fuel_gauge_init();
	app_charger_init();
	app_power_init();

	if (app_text_init(APP_TEXT_FONT_TAMZEN) == 0) {
		app_footer_init();

		app_power_show_wake_message();

		app_footer_sample_current();
		app_header_update(HEADER_FOLDER_PLACEHOLDER);
		app_footer_draw();
		app_text_full_refresh();

		/*
		 * Mount the SD card once at boot - app_sd_init()/
		 * app_sd_deinit() pair unconditionally regardless of mount
		 * success, since a failed mount can still leave the card's
		 * power hold in place (see sd.h) and needs releasing either
		 * way. Every later display refresh brackets its own SD power
		 * independently (app_display_lock()/app_display_unlock(),
		 * see sd.h), so nothing here needs to keep the card mounted
		 * or powered beyond this one-shot check.
		 */
		app_sd_init();
		app_sd_deinit();

		/*
		 * From here on, nothing touches text.c/display.c from the main
		 * thread again - it's all event-driven, and every path is
		 * funneled onto the single system workqueue thread so nothing
		 * races the shared framebuffer/dirty-row state: the RTC alarm
		 * callback (rtc.c's alarm_trampoline) and the charger STAT
		 * callback (charger.c's stat_change_work_handler) both already
		 * run there, and the footer's periodic cycle below is a
		 * k_work_delayable chain instead of a main-thread sleep loop
		 * for the same reason. main() falls through and returns after
		 * this - Zephyr doesn't need it to keep running.
		 */
		k_work_init_delayable(&footer_sample_work, footer_sample_handler);
		k_work_init_delayable(&footer_draw_work, footer_draw_handler);
		k_work_schedule(&footer_sample_work,
				K_SECONDS(STATUS_REFRESH_INTERVAL_SEC -
					  STATUS_CURRENT_SAMPLE_LEAD_SEC));

		/*
		 * mask = SECOND only, with tm_sec = 0, leaves minute/hour/day
		 * unconstrained - the PCF85063 driver disables those fields
		 * entirely (see pcf85063a_alarm_set_time()) rather than
		 * matching a fixed value, so this alarm matches - and
		 * therefore fires - every time seconds rolls to 0, forever
		 * recurring, not a one-shot needing to be re-armed.
		 */
		struct rtc_time minute_tick = {.tm_sec = 0};

		app_rtc_set_alarm(&minute_tick, RTC_ALARM_TIME_MASK_SECOND, on_header_event);
		app_charger_set_change_callback(on_header_event);
	}

	return 0;
}
