/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>

#include "browser.h"
#include "buttons.h"
#include "charger.h"
#include "display.h"
#include "footer.h"
#include "fuel_gauge.h"
#include "header.h"
#include "power.h"
#include "rtc.h"
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

/* app_header_update()'s "folder" argument (header.h) when there's no real
 * browser path to show - the browser failed to mount the SD card at boot,
 * or hasn't been initialized yet.
 */
#define HEADER_FOLDER_FALLBACK "(no SD card)"

/*
 * The footer's periodic sample-then-draw cycle, as a k_work_delayable
 * chain on the system workqueue instead of a main-thread sleep loop -
 * see the comment in main() for why everything that touches text.c's
 * framebuffer needs to live on one thread.
 */
static struct k_work_delayable footer_sample_work;
static struct k_work_delayable footer_draw_work;

/* Set once app_browser_init() succeeds in main() - see on_header_event()
 * and the HEADER_FOLDER_FALLBACK comment above.
 */
static bool browser_ready;

/*
 * Registered with both app_rtc_set_alarm() (fires once a minute) and
 * app_charger_set_change_callback() (fires on every STAT transition) -
 * see header.h for exactly what app_header_update() checks before
 * deciding whether there's actually anything new to draw. browser.c also
 * calls app_header_update() directly on every folder navigation, so the
 * folder text doesn't lag behind until the next one of these ticks.
 */
static void on_header_event(void)
{
	app_fuel_gauge_notify_charger_state(app_charger_is_charging());

	const char *folder = browser_ready ? app_browser_get_current_path()
					    : HEADER_FOLDER_FALLBACK;

	if (app_header_update(folder) > 0) {
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
	printk("NanoPage: RTC + buttons + display + fuel gauge + file browser bring-up.\n");

	if (app_rtc_init()) {
		return 0;
	}

	app_display_init();
	app_fuel_gauge_init();
	app_charger_init();
	app_power_init();
	app_buttons_init();

	if (app_text_init(APP_TEXT_FONT_TAMZEN) == 0) {
		app_footer_init();

		app_power_show_wake_message();

		app_footer_sample_current();
		app_footer_draw();

		/*
		 * Mounts the SD card and leaves it mounted for as long as the
		 * browser stays active (see sd.h/browser.h - unlike the old
		 * one-shot boot-time mount check this replaces, a live file
		 * browser needs to keep listing directories on demand, not
		 * just prove the card works once). Draws the initial listing
		 * plus the header (with the real starting path) and does the
		 * one full refresh that covers the footer drawn just above
		 * too - avoids a second, redundant full-panel flash right
		 * after it. On failure (no card inserted, bad mount), falls
		 * back to a plain header + full refresh so the footer at
		 * least isn't left unflushed.
		 */
		browser_ready = (app_browser_init("/") == 0);
		if (!browser_ready) {
			printk("main: browser init failed - no SD card?\n");
			app_header_update(HEADER_FOLDER_FALLBACK);
			app_text_full_refresh();
		}

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
