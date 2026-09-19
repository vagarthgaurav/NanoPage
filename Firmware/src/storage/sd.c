/* SPDX-License-Identifier: Apache-2.0 */

#include <ff.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/sys/printk.h>

#include "config.h"
#include "display.h"
#include "sd.h"

#define DISK_MOUNT_PT "/" APP_SD_DISK_NAME ":"

static const struct device *const sd_regulator = DEVICE_DT_GET(DT_NODELABEL(regulator_sdhc));

static const struct gpio_dt_spec sd_cs_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi3),
									cs_gpios, 1);

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = DISK_MOUNT_PT,
};

static bool sd_mounted;

static bool sd_usb_claimed;

static int sd_power_refcount;

int app_sd_power_on(void)
{
	int ret;

	if (sd_power_refcount > 0) {
		sd_power_refcount++;
		return 0;
	}

	if (!device_is_ready(sd_regulator)) {
		printk("SD: power switch device not ready\n");
		return -ENODEV;
	}

	ret = regulator_enable(sd_regulator);
	if (ret) {
		printk("SD: failed to enable power switch (%d)\n", ret);
		return ret;
	}

	k_msleep(APP_SD_POWER_UP_DELAY_MS);

	sd_power_refcount = 1;

	return 0;
}

void app_sd_power_off(void)
{
	if (sd_power_refcount == 0) {
		return;
	}

	sd_power_refcount--;

	if (sd_power_refcount == 0) {
		gpio_pin_set_dt(&sd_cs_gpio, 1);
		regulator_disable(sd_regulator);
	}
}

static int sd_power_up_before_display_init(void)
{
	return app_sd_power_on();
}
SYS_INIT(sd_power_up_before_display_init, POST_KERNEL, 84);

static int sd_power_down_after_display_init(void)
{
	const struct gpio_dt_spec busy = GPIO_DT_SPEC_GET(DT_CHOSEN(zephyr_display), busy_gpios);

	while (gpio_pin_get_dt(&busy) > 0) {
		k_msleep(1);
	}

	app_sd_power_off();

	return 0;
}
SYS_INIT(sd_power_down_after_display_init, POST_KERNEL, 86);

int app_sd_init(void)
{
	int ret;

	if (sd_usb_claimed) {
		return -EBUSY;
	}

	app_display_lock();

	ret = app_sd_power_on();
	if (ret) {
		app_display_unlock();
		return ret;
	}

	ret = fs_mount(&mp);
	if (ret) {
		printk("SD: mount failed (%d)\n", ret);
		app_display_unlock();
		return ret;
	}

	sd_mounted = true;
	printk("SD: mounted at %s\n", DISK_MOUNT_PT);

	app_display_unlock();

	return 0;
}

void app_sd_deinit(void)
{
	app_display_lock();

	if (sd_mounted) {
		fs_unmount(&mp);
		sd_mounted = false;
	}

	app_sd_power_off();

	app_display_unlock();
}

int app_sd_usb_claim(void)
{
	int ret;

	app_display_lock();

	sd_usb_claimed = true;

	ret = app_sd_power_on();
	if (ret) {
		return ret;
	}

	ret = disk_access_init(APP_SD_DISK_NAME);
	if (ret) {
		printk("SD: USB disk_access_init failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

void app_sd_usb_release(void)
{
	disk_access_ioctl(APP_SD_DISK_NAME, DISK_IOCTL_CTRL_DEINIT, NULL);

	app_sd_power_off();

	sd_usb_claimed = false;

	app_display_unlock();
}

int app_sd_full_path(const char *path, char *out, size_t out_size)
{
	int ret;

	if (!sd_mounted) {
		return -ENODEV;
	}

	ret = snprintf(out, out_size, "%s%s", DISK_MOUNT_PT, path);
	if (ret < 0 || (size_t)ret >= out_size) {
		return -ENAMETOOLONG;
	}

	return 0;
}

int app_sd_list_dir(const char *path, struct app_sd_dirent *entries, size_t max_entries,
		     size_t *count)
{
	char full_path[APP_SD_MAX_PATH_LEN];
	struct fs_dir_t dir;
	struct fs_dirent dirent;
	int ret;

	*count = 0;

	ret = app_sd_full_path(path, full_path, sizeof(full_path));
	if (ret) {
		return ret;
	}

	fs_dir_t_init(&dir);

	ret = fs_opendir(&dir, full_path);
	if (ret) {
		printk("SD: opendir(%s) failed (%d)\n", full_path, ret);
		return ret;
	}

	while (*count < max_entries) {
		ret = fs_readdir(&dir, &dirent);
		if (ret || dirent.name[0] == '\0') {
			break;
		}

		if (dirent.name[0] == '.') {
			continue; /* "." / ".." - FAT/LFN has no other dot-files */
		}

		strncpy(entries[*count].name, dirent.name, APP_SD_MAX_NAME_LEN - 1);
		entries[*count].name[APP_SD_MAX_NAME_LEN - 1] = '\0';
		entries[*count].is_dir = (dirent.type == FS_DIR_ENTRY_DIR);
		(*count)++;
	}

	fs_closedir(&dir);

	return 0;
}
