/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>

#include "i2c1_bus.h"
#include "rtc.h"

static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc0));

static app_rtc_alarm_cb_t alarm_user_cb;

static const char *const weekday_name[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat",
};

int app_rtc_init(void)
{
	if (!device_is_ready(rtc_dev)) {
		printk("RTC device not ready\n");
		return -ENODEV;
	}

	/*
	 * A System OFF wake is a full reboot, so this runs on every wake too
	 * - but the PCF85063 keeps ticking through System OFF off its own
	 * always-on rail, independent of the MCU. Unconditionally reseeding
	 * here (as this used to) stomped that real, still-correct time back
	 * to the hardcoded reference below on every wake, which is why the
	 * clock appeared to reset to 12:00 after sleeping. Zephyr's PCF85063A
	 * driver returns -ENODATA from rtc_get_time() specifically when the
	 * chip's own OS (oscillator-stopped) flag is set - genuinely invalid
	 * time, e.g. true first power-up - so only seed in that case and
	 * leave an already-ticking clock alone otherwise.
	 */
	struct rtc_time now;
	int ret;

	app_i2c1_lock();
	ret = rtc_get_time(rtc_dev, &now);
	if (ret != -ENODATA) {
		app_i2c1_unlock();
		return 0;
	}

	/*
	 * PCF85063 powers up with no valid time set - seed it once at boot
	 * to a fixed reference so the ticking is visible during bring-up.
	 * Replace with a real time source once one exists.
	 */
	struct rtc_time set_time = {
		.tm_year = 2026 - 1900,
		.tm_mon = 8, /* 0-indexed: September */
		.tm_mday = 10,
		.tm_hour = 12,
		.tm_min = 0,
		.tm_sec = 0,
		.tm_wday = 4, /* 2026-09-10 is a Thursday */
	};

	ret = rtc_set_time(rtc_dev, &set_time);
	app_i2c1_unlock();

	if (ret) {
		printk("rtc_set_time failed: %d\n", ret);
	}

	return ret;
}

int app_rtc_get_time(struct rtc_time *now)
{
	int ret;

	app_i2c1_lock();
	ret = rtc_get_time(rtc_dev, now);
	app_i2c1_unlock();

	return ret;
}

void app_rtc_log_time(void)
{
	struct rtc_time now;
	int ret = app_rtc_get_time(&now);

	if (ret) {
		printk("rtc_get_time failed: %d\n", ret);
		return;
	}

	printk("rtc: %04d-%02d-%02d %02d:%02d:%02d\n",
	       now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
	       now.tm_hour, now.tm_min, now.tm_sec);
}

int app_rtc_format_time(char *buf, size_t len)
{
	struct rtc_time now;
	int ret = app_rtc_get_time(&now);

	if (ret) {
		return ret;
	}

	snprintf(buf, len, "%02d:%02d", now.tm_hour, now.tm_min);
	return 0;
}

int app_rtc_format_date(char *buf, size_t len)
{
	struct rtc_time now;
	int ret = app_rtc_get_time(&now);

	if (ret) {
		return ret;
	}

	snprintf(buf, len, "%s, %04d-%02d-%02d", weekday_name[now.tm_wday % 7],
		 now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
	return 0;
}

int app_rtc_format_datetime_iso(char *buf, size_t len)
{
	struct rtc_time now;
	int ret = app_rtc_get_time(&now);

	if (ret) {
		return ret;
	}

	snprintf(buf, len, "%04d-%02d-%02d %02d:%02d:%02d", now.tm_year + 1900, now.tm_mon + 1,
		 now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
	return 0;
}

/*
 * Bridges the driver's rtc_alarm_callback signature to our simpler
 * app_rtc_alarm_cb_t. Runs on the system workqueue (see callback_work_handler
 * in rtc_pcf85063a.c), not in ISR context, so it's safe to do normal work
 * (logging, waking up a display refresh, ...) from here.
 */
static void alarm_trampoline(const struct device *dev, uint16_t id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);

	/*
	 * INT1 is open-drain/active-low and the driver watches for a falling
	 * edge. Until the alarm-pending (AF) flag is cleared the pin stays
	 * asserted low, so the next alarm match won't produce a new edge and
	 * the callback would never fire again. rtc_alarm_is_pending() both
	 * reports and clears AF - call it every time to re-arm the line.
	 */
	app_i2c1_lock();
	rtc_alarm_is_pending(rtc_dev, 0);
	app_i2c1_unlock();

	if (alarm_user_cb) {
		alarm_user_cb();
	}
}

int app_rtc_set_alarm(const struct rtc_time *time, uint16_t mask, app_rtc_alarm_cb_t cb)
{
	int ret;

	alarm_user_cb = cb;

	app_i2c1_lock();

	ret = rtc_alarm_set_time(rtc_dev, 0, mask, time);
	if (ret) {
		app_i2c1_unlock();
		printk("rtc_alarm_set_time failed: %d\n", ret);
		return ret;
	}

	ret = rtc_alarm_set_callback(rtc_dev, 0, alarm_trampoline, NULL);
	app_i2c1_unlock();

	if (ret) {
		printk("rtc_alarm_set_callback failed: %d\n", ret);
		return ret;
	}

	return 0;
}

int app_rtc_set_daily_alarm(uint8_t hour, uint8_t min, app_rtc_alarm_cb_t cb)
{
	struct rtc_time alarm_time = {
		.tm_hour = hour,
		.tm_min = min,
	};

	return app_rtc_set_alarm(&alarm_time, RTC_ALARM_TIME_MASK_HOUR | RTC_ALARM_TIME_MASK_MINUTE,
				  cb);
}

int app_rtc_clear_alarm(void)
{
	struct rtc_time none = {0};
	int ret;

	alarm_user_cb = NULL;

	/* mask = 0 disables every alarm field, so it can never match again. */
	app_i2c1_lock();
	ret = rtc_alarm_set_time(rtc_dev, 0, 0, &none);
	app_i2c1_unlock();

	return ret;
}
