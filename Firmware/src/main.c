/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>

#include "browser.h"
#include "buttons.h"
#include "charger.h"
#include "config.h"
#include "currentlog.h"
#include "display.h"
#include "footer.h"
#include "fuel_gauge.h"
#include "header.h"
#include "menu.h"
#include "power.h"
#include "rtc.h"
#include "settings.h"
#include "text.h"
#include "usb_msc.h"

/* app_header_update()'s "folder" argument (header.h) when there's no real
 * browser path to show - the browser failed to mount the SD card, or hasn't
 * been launched from the app menu (menu.h) yet this boot.
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

/*
 * Registered with both app_rtc_set_alarm() (fires once a minute) and
 * app_charger_set_change_callback() (fires on every STAT transition) -
 * see header.h for exactly what app_header_update() checks before
 * deciding whether there's actually anything new to draw. browser.c/menu.c/
 * settings.c also call app_header_update() directly on every screen
 * transition, so the label doesn't lag behind until the next one of these
 * ticks - this is just the periodic path, and has to pick between the same
 * mutually-exclusive "what's on screen right now" sources those direct
 * calls do.
 */
static void on_header_event(void)
{
	app_fuel_gauge_notify_charger_state(app_charger_is_charging());

	const char *folder;

	if (app_usb_msc_is_active()) {
		folder = "Connected to computer";
	} else if (app_menu_is_active()) {
		folder = "Apps";
	} else if (app_settings_is_active()) {
		folder = "Settings";
	} else if (app_browser_is_ready()) {
		folder = app_browser_get_current_path();
	} else {
		folder = HEADER_FOLDER_FALLBACK;
	}

	if (app_header_update(folder) > 0) {
		app_text_flush();
	}
}

static void footer_draw_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_footer_draw();
	app_text_flush();

	struct app_footer_reading reading;

	app_footer_get_last(&reading);
	app_currentlog_write(reading.instant_current_ua, reading.avg_current_ua,
			      reading.voltage_uv);

	if (app_fuel_gauge_should_save_learned_params()) {
		app_fuel_gauge_save_learned_params();
	}

	k_work_schedule(&footer_sample_work,
			K_SECONDS(APP_STATUS_REFRESH_INTERVAL_SEC - APP_STATUS_CURRENT_SAMPLE_LEAD_SEC));
}

static void footer_sample_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	app_footer_sample_current();
	k_work_schedule(&footer_draw_work, K_SECONDS(APP_STATUS_CURRENT_SAMPLE_LEAD_SEC));
}

int main(void)
{
	printk("NanoPage: RTC + buttons + display + fuel gauge + app menu bring-up.\n");

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

		app_footer_sample_current();
		app_footer_draw();

		/*
		 * Boots into the app menu (menu.h) rather than straight into
		 * the file browser - the browser is now just one of the
		 * apps listed there, launched (and its SD card mounted) only
		 * once the user actually selects it. This also covers the
		 * footer drawn just above with the one full refresh the menu
		 * does anyway, avoiding a second, redundant full-panel flash
		 * right after it.
		 */
		app_menu_show();

		/*
		 * Registers for VBUS notifications but doesn't attach to the
		 * bus yet (see usb_msc.h) - safe to call regardless of which
		 * screen is currently active, and needs to run after
		 * app_menu_show() since a USB disconnect later re-enters
		 * whatever browser session app_usb_msc_init()'s eventual
		 * app_browser_reinit() call expects to already exist (see
		 * usb_msc.c's release_session()).
		 */
		if (app_usb_msc_init()) {
			printk("main: USB Mass Storage init failed\n");
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
				K_SECONDS(APP_STATUS_REFRESH_INTERVAL_SEC -
					  APP_STATUS_CURRENT_SAMPLE_LEAD_SEC));

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
