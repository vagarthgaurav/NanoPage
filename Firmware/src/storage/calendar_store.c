/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "calendar_store.h"
#include "companion_proto.h"
#include "sd.h"
#include "usb_msc.h"

#define CAL_DIR_REL "/.nanopage"
#define CAL_BIN_REL CAL_DIR_REL "/calendar.bin"
#define CAL_TMP_REL CAL_DIR_REL "/calendar.tmp"

/* The loaded blob. load() runs on the UI thread and get_day() is only called
 * by the day view after it, so no locking is needed.
 */
static uint8_t blob_buf[APP_CALENDAR_STORE_MAX_BLOB];
static size_t loaded_len;
static uint16_t loaded_count;
static bool loaded;

/* FIL carries a 512 byte sector buffer and the paths are long, keep them off
 * the stack. save() runs on the companion library thread and load() on the UI
 * thread, so io_lock serialises them over these statics.
 */
static K_MUTEX_DEFINE(io_lock);
static struct fs_file_t file;
static char bin_path[APP_SD_MAX_PATH_LEN];
static char tmp_path[APP_SD_MAX_PATH_LEN];
static char dir_path[APP_SD_MAX_PATH_LEN];

int app_calendar_store_validate(const uint8_t *blob, size_t len, uint16_t expected_count)
{
	struct app_companion_cal_header hdr;
	size_t pos = sizeof(hdr);

	if (len < sizeof(hdr)) {
		return -EINVAL;
	}

	memcpy(&hdr, blob, sizeof(hdr));

	if (memcmp(hdr.magic, APP_COMPANION_CAL_MAGIC, sizeof(hdr.magic)) != 0 ||
	    hdr.version != APP_COMPANION_CAL_VERSION || hdr.event_count != expected_count ||
	    hdr.event_count > APP_CALENDAR_STORE_MAX_EVENTS) {
		return -EINVAL;
	}

	for (uint16_t i = 0; i < hdr.event_count; i++) {
		struct app_companion_cal_event ev;

		if (len - pos < sizeof(ev)) {
			return -EINVAL;
		}
		memcpy(&ev, blob + pos, sizeof(ev));
		pos += sizeof(ev);

		if (len - pos < ev.title_len) {
			return -EINVAL;
		}
		for (size_t c = 0; c < ev.title_len; c++) {
			if (blob[pos + c] < 32 || blob[pos + c] > 126) {
				return -EINVAL;
			}
		}
		pos += ev.title_len;
	}

	return (pos == len) ? 0 : -EINVAL;
}

/* Resolves both file paths. Needs the card mounted. */
static int resolve_paths(char *bin, char *tmp, size_t size)
{
	int ret = app_sd_full_path(CAL_BIN_REL, bin, size);

	if (ret) {
		return ret;
	}

	return app_sd_full_path(CAL_TMP_REL, tmp, size);
}

static int write_tmp(const char *tmp, const uint8_t *blob, size_t len)
{
	ssize_t n;
	int ret;

	fs_file_t_init(&file);
	ret = fs_open(&file, tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret) {
		return ret;
	}

	n = fs_write(&file, blob, len);
	if (n < 0) {
		ret = (int)n;
	} else if ((size_t)n != len) {
		ret = -EIO;
	} else {
		ret = fs_sync(&file);
	}

	/* Close even on error; keep the first error. */
	int close_ret = fs_close(&file);

	return ret ? ret : close_ret;
}

static int save_locked(const uint8_t *blob, size_t len)
{
	int ret;

	ret = resolve_paths(bin_path, tmp_path, sizeof(bin_path));
	if (ret == 0) {
		ret = app_sd_full_path(CAL_DIR_REL, dir_path, sizeof(dir_path));
	}
	if (ret) {
		return ret;
	}

	ret = fs_mkdir(dir_path);
	if (ret && ret != -EEXIST) {
		printk("calendar: mkdir failed (%d)\n", ret);
		return ret;
	}

	ret = write_tmp(tmp_path, blob, len);
	if (ret) {
		printk("calendar: write failed (%d)\n", ret);
		fs_unlink(tmp_path);
		return ret;
	}

	/* FatFs cannot rename onto an existing file. If power dies between the
	 * unlink and the rename, load() falls back to the complete calendar.tmp.
	 */
	ret = fs_unlink(bin_path);
	if (ret && ret != -ENOENT) {
		printk("calendar: unlink failed (%d)\n", ret);
		return ret;
	}

	ret = fs_rename(tmp_path, bin_path);
	if (ret) {
		printk("calendar: rename failed (%d)\n", ret);
	}

	return ret;
}

int app_calendar_store_save(const uint8_t *blob, size_t len)
{
	int ret;

	if (app_usb_msc_is_active()) {
		return -EBUSY;
	}

	k_mutex_lock(&io_lock, K_FOREVER);

	ret = app_sd_init();
	if (ret == 0) {
		ret = save_locked(blob, len);
		app_sd_deinit();
	}

	k_mutex_unlock(&io_lock);

	return ret;
}

/* Reads the whole file at `path` into blob_buf. Returns bytes read or <0. */
static ssize_t read_file(const char *path)
{
	struct fs_dirent st;
	size_t total = 0;
	int ret;

	ret = fs_stat(path, &st);
	if (ret) {
		return ret;
	}
	if (st.size > sizeof(blob_buf)) {
		return -EINVAL;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, path, FS_O_READ);
	if (ret) {
		return ret;
	}

	while (total < st.size) {
		ssize_t n = fs_read(&file, blob_buf + total, st.size - total);

		if (n < 0) {
			fs_close(&file);
			return n;
		}
		if (n == 0) {
			break;
		}
		total += n;
	}
	fs_close(&file);

	/* Shorter than fs_stat said: the validation below will reject it. */
	return (ssize_t)total;
}

static int load_locked(void)
{
	struct app_companion_cal_header hdr;
	ssize_t n;
	int ret;

	ret = resolve_paths(bin_path, tmp_path, sizeof(bin_path));
	if (ret) {
		return ret;
	}

	n = read_file(bin_path);
	if (n == -ENOENT) {
		n = read_file(tmp_path);
	}
	if (n < 0) {
		return (int)n;
	}

	if ((size_t)n < sizeof(hdr)) {
		return -EINVAL;
	}
	memcpy(&hdr, blob_buf, sizeof(hdr));

	ret = app_calendar_store_validate(blob_buf, (size_t)n, hdr.event_count);
	if (ret) {
		return ret;
	}

	loaded_len = (size_t)n;
	loaded_count = hdr.event_count;
	loaded = true;

	return hdr.event_count;
}

int app_calendar_store_load(void)
{
	int ret;

	loaded = false;
	loaded_len = 0;
	loaded_count = 0;

	if (app_usb_msc_is_active()) {
		return -EBUSY;
	}

	k_mutex_lock(&io_lock, K_FOREVER);

	ret = app_sd_init();
	if (ret == 0) {
		ret = load_locked();
		app_sd_deinit();
	}

	k_mutex_unlock(&io_lock);

	if (ret < 0) {
		loaded = false;
		loaded_len = 0;
		loaded_count = 0;
	}

	return ret;
}

size_t app_calendar_store_get_day(int32_t days_since_2000, struct app_calendar_store_event *out,
				  size_t max)
{
	size_t n = 0;

	if (!loaded) {
		return 0;
	}

	/* Pass 0 collects all-day events, pass 1 the timed ones, so a full
	 * output array drops timed events first.
	 */
	for (int pass = 0; pass < 2; pass++) {
		size_t pos = sizeof(struct app_companion_cal_header);

		for (uint16_t i = 0; i < loaded_count && n < max; i++) {
			struct app_companion_cal_event ev;
			const char *title;
			bool all_day;
			size_t tlen, j;

			/* Bounds already guaranteed by validate() at load time. */
			memcpy(&ev, blob_buf + pos, sizeof(ev));
			title = (const char *)blob_buf + pos + sizeof(ev);
			pos += sizeof(ev) + ev.title_len;

			if ((int32_t)(ev.start_local / APP_CALENDAR_STORE_MINUTES_PER_DAY) !=
			    days_since_2000) {
				continue;
			}

			all_day = (ev.flags & APP_COMPANION_CAL_FLAG_ALL_DAY) != 0;
			if (all_day != (pass == 0)) {
				continue;
			}

			uint16_t start = all_day ? 0 : (ev.start_local %
							APP_CALENDAR_STORE_MINUTES_PER_DAY);

			/* Stable insertion: after every entry that starts at or before. */
			j = n;
			while (!all_day && j > 0 && !out[j - 1].all_day &&
			       out[j - 1].start_min > start) {
				out[j] = out[j - 1];
				j--;
			}

			tlen = MIN((size_t)ev.title_len, (size_t)APP_CALENDAR_STORE_TITLE_MAX);
			out[j].start_min = start;
			out[j].dur_min = ev.duration_min;
			out[j].all_day = all_day;
			memcpy(out[j].title, title, tlen);
			out[j].title[tlen] = '\0';
			n++;
		}
	}

	return n;
}
