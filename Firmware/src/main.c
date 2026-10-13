/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/kernel.h>

#include "bookmarks.h"
#include "browser.h"
#include "buttons.h"
#include "calendar_day.h"
#include "charger.h"
#include "config.h"
#include "display.h"
#include "footer.h"
#include "fuel_gauge.h"
#include "header.h"
#include "menu.h"
#include "power.h"
#include "reader.h"
#include "reader_menu.h"
#include "rtc.h"
#include "settings.h"
#include "text.h"
#include "toc.h"
#include "usb_msc.h"

#define HEADER_FOLDER_FALLBACK "(no SD card)"

static struct k_work_delayable footer_sample_work;
static struct k_work_delayable footer_draw_work;

static void on_header_event(void)
{
	app_fuel_gauge_notify_charger_state(app_charger_is_charging());

	const char *folder;
	bool bookmarked = false;

	if (app_usb_msc_is_active()) {
		folder = "Connected to computer";
	} else if (app_menu_is_active()) {
		folder = "Apps";
	} else if (app_settings_is_active()) {
		folder = "Settings";
	} else if (app_calendar_day_is_active()) {
		static char date[24];

		if (app_calendar_day_format_date(date, sizeof(date)) != 0) {
			snprintf(date, sizeof(date), "Calendar");
		}
		folder = date;
	} else if (app_reader_menu_is_active()) {
		folder = "Menu";
	} else if (app_toc_is_active()) {
		folder = "Table of Contents";
	} else if (app_bookmarks_is_active()) {
		folder = "Bookmarks";
	} else if (app_reader_is_active()) {
		folder = "";
		bookmarked = app_reader_current_page_bookmarked();
	} else if (app_browser_is_ready()) {
		folder = app_browser_get_current_path();
	} else {
		folder = HEADER_FOLDER_FALLBACK;
	}

	if (app_header_update(folder, bookmarked) > 0) {
		app_text_flush();
	}
}

static void footer_draw_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!app_usb_msc_is_active()) {
		app_footer_draw();
		app_text_flush();
	}

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

		if (!app_power_woke_from_system_off() ||
		    app_reader_resume_last_session(app_menu_show) != 0) {
			app_menu_show();
		}

		if (app_usb_msc_init()) {
			printk("main: USB Mass Storage init failed\n");
		}

		k_work_init_delayable(&footer_sample_work, footer_sample_handler);
		k_work_init_delayable(&footer_draw_work, footer_draw_handler);
		k_work_schedule(&footer_sample_work,
				K_SECONDS(APP_STATUS_REFRESH_INTERVAL_SEC -
					  APP_STATUS_CURRENT_SAMPLE_LEAD_SEC));

		struct rtc_time minute_tick = {.tm_sec = 0};

		app_rtc_set_alarm(&minute_tick, RTC_ALARM_TIME_MASK_SECOND, on_header_event);
		app_charger_set_change_callback(on_header_event);
	}

	return 0;
}
