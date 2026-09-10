/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/rtc.h>

#include "rtc.h"

static const struct device *const rtc_dev = DEVICE_DT_GET(DT_ALIAS(rtc0));

int app_rtc_init(void)
{
	if (!device_is_ready(rtc_dev)) {
		printk("RTC device not ready\n");
		return -ENODEV;
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
	};

	int ret = rtc_set_time(rtc_dev, &set_time);

	if (ret) {
		printk("rtc_set_time failed: %d\n", ret);
	}

	return ret;
}

void app_rtc_log_time(void)
{
	struct rtc_time now;
	int ret = rtc_get_time(rtc_dev, &now);

	if (ret) {
		printk("rtc_get_time failed: %d\n", ret);
		return;
	}

	printk("rtc: %04d-%02d-%02d %02d:%02d:%02d\n",
	       now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
	       now.tm_hour, now.tm_min, now.tm_sec);
}
