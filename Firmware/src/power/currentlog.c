/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stdio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/printk.h>

#include "currentlog.h"
#include "rtc.h"
#include "sd.h"

#define APP_CURRENTLOG_PATH "/currentlog.csv"
#define APP_CURRENTLOG_HEADER "timestamp,instant_current_ma,avg_current_ma,voltage_v\n"

/* Formats a signed microamp current as "-12.345" (mA, 3 decimal places) -
 * same convention as footer.c's own format_current_ma(), duplicated rather
 * than shared since this is a CSV numeric field (no unit suffix), not
 * on-screen text.
 */
static void format_signed_ma(char *out, size_t len, int32_t current_ua)
{
	bool neg = current_ua < 0;

	if (neg) {
		current_ua = -current_ua;
	}

	snprintf(out, len, "%s%d.%03d", neg ? "-" : "", (int)(current_ua / 1000),
		 (int)(current_ua % 1000));
}

void app_currentlog_write(int32_t instant_current_ua, int32_t avg_current_ua, int64_t voltage_uv)
{
	char full_path[APP_SD_MAX_PATH_LEN];
	char timestamp[20];
	char instant_buf[16];
	char avg_buf[16];
	char line[128];
	struct fs_dirent stat;
	struct fs_file_t f;
	bool new_file;
	int len;
	int ret;

	if (app_rtc_format_datetime_iso(timestamp, sizeof(timestamp))) {
		return;
	}

	/*
	 * Brackets the card's own power/mount for just this one write - see
	 * sd.h's usage note on app_sd_init()/app_sd_deinit(). This runs on
	 * every status-bar tick regardless of whether the browser/reader are
	 * otherwise touching the card, so without this bracket the logger
	 * itself would be the reason the card never goes idle.
	 */
	if (app_sd_init()) {
		return; /* no SD card - nothing to log */
	}

	if (app_sd_full_path(APP_CURRENTLOG_PATH, full_path, sizeof(full_path))) {
		app_sd_deinit();
		return;
	}

	/* fs_stat() failing (most likely -ENOENT) means this is the first
	 * ever write - write a header row once so the file is a plain CSV.
	 */
	new_file = (fs_stat(full_path, &stat) != 0);

	fs_file_t_init(&f);
	ret = fs_open(&f, full_path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (ret) {
		printk("currentlog: failed to open %s (%d)\n", full_path, ret);
		app_sd_deinit();
		return;
	}

	if (new_file) {
		fs_write(&f, APP_CURRENTLOG_HEADER, sizeof(APP_CURRENTLOG_HEADER) - 1);
	}

	format_signed_ma(instant_buf, sizeof(instant_buf), instant_current_ua);
	format_signed_ma(avg_buf, sizeof(avg_buf), avg_current_ua);

	len = snprintf(line, sizeof(line), "%s,%s,%s,%d.%03d\n", timestamp, instant_buf, avg_buf,
		       (int)(voltage_uv / 1000000), (int)((voltage_uv / 1000) % 1000));

	if (len > 0 && (size_t)len < sizeof(line)) {
		ret = fs_write(&f, line, (size_t)len);
		if (ret < 0 || (size_t)ret != (size_t)len) {
			printk("currentlog: write failed (%d)\n", ret);
		}
	}

	fs_close(&f);
	app_sd_deinit();
}
