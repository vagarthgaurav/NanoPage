/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/sys/timeutil.h>

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

	struct rtc_time now;
	int ret;

	app_i2c1_lock();
	ret = rtc_get_time(rtc_dev, &now);
	if (ret == 0) {
		app_i2c1_unlock();
		return 0;
	}
	if (ret != -ENODATA) {
		app_i2c1_unlock();
		printk("rtc_get_time failed: %d\n", ret);
		return ret;
	}

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

/* Sakamoto's day-of-week algorithm; the PCF85063A stores tm_wday verbatim and
 * app_rtc_format_date() reads it back, so it has to be right.
 */
static int day_of_week(int year, int month, int day)
{
	static const int offset[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

	if (month < 3) {
		year--;
	}

	return (year + year / 4 - year / 100 + year / 400 + offset[month - 1] + day) % 7;
}

int app_rtc_set_time(const struct rtc_time *t)
{
	struct rtc_time set_time = {
		.tm_year = t->tm_year,
		.tm_mon = t->tm_mon,
		.tm_mday = t->tm_mday,
		.tm_hour = t->tm_hour,
		.tm_min = t->tm_min,
		.tm_sec = t->tm_sec,
	};
	int ret;

	if (t->tm_mon < 0 || t->tm_mon > 11) {
		return -EINVAL;
	}

	set_time.tm_wday = day_of_week(t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

	app_i2c1_lock();
	ret = rtc_set_time(rtc_dev, &set_time);
	app_i2c1_unlock();

	if (ret) {
		printk("rtc_set_time failed: %d\n", ret);
	}

	return ret;
}

#define SECONDS_1970_TO_2000 946684800LL

int app_rtc_get_minutes_since_2000(uint32_t *out)
{
	struct rtc_time now;
	struct tm tm = {
		.tm_sec = 0,
	};
	int64_t secs;
	int ret = app_rtc_get_time(&now);

	if (ret) {
		return ret;
	}

	tm.tm_sec = now.tm_sec;
	tm.tm_min = now.tm_min;
	tm.tm_hour = now.tm_hour;
	tm.tm_mday = now.tm_mday;
	tm.tm_mon = now.tm_mon;
	tm.tm_year = now.tm_year;

	secs = timeutil_timegm64(&tm) - SECONDS_1970_TO_2000;
	if (secs < 0) {
		return -ERANGE;
	}

	*out = (uint32_t)(secs / 60);
	return 0;
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

static void alarm_trampoline(const struct device *dev, uint16_t id, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(user_data);

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
