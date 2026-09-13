/* SPDX-License-Identifier: Apache-2.0 */

#include <ff.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/fs/fs.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "config.h"
#include "display.h"
#include "sd.h"

/*
 * APP_SD_POWER_UP_DELAY_MS (config.h): cards typically settle within a
 * few ms to low tens of ms after power-up; 20ms leaves margin over that
 * without paying the cost of the original, more conservative 100ms on
 * every refresh (this delay is now incurred far more often than "once
 * per session" - see the per-refresh bracketing note in sd.h, so its
 * cost actually matters for battery life). Revisit upward if mount/read
 * ever proves flaky against a real card.
 */

#define DISK_DRIVE_NAME "SD"
#define DISK_MOUNT_PT "/" DISK_DRIVE_NAME ":"

static const struct device *const sd_regulator = DEVICE_DT_GET(DT_NODELABEL(regulator_sdhc));

/*
 * The SD card's own SPI CS line - cs-gpios[1] on &spi3 in nanopage.dts
 * (P1.09), pulled directly out of that property rather than needing a
 * separate devicetree node just to name it. Zephyr's SPI driver only
 * ever drives this pin during an actual transaction; the rest of the
 * time it sits wherever the driver last left it, which per spi_context's
 * own convention is deselected - physically HIGH, since this is
 * GPIO_ACTIVE_LOW. That's a real, continuous ~3.3V presented to the
 * card's CS pin even while its own VDD (+3V3_SD) is switched off,
 * measured forward-biasing the card's CS-pin ESD diode into its VDD
 * rail hard enough to hold +3V3_SD at ~0.83V (a single diode drop) and
 * draw ~7mA continuously, even at rest (confirmed on hardware
 * 2026-09-13). app_sd_power_off() below forces this pin low right
 * before cutting power specifically to kill that path - the SPI driver
 * re-asserts it correctly on its own before the next real transaction,
 * so nothing needs to restore it on power-on.
 */
static const struct gpio_dt_spec sd_cs_gpio = GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi3),
									cs_gpios, 1);

/*
 * A boot-time SYS_INIT hook that force-drove this pin low immediately
 * after spi3's own init (which otherwise leaves it deasserted/physically
 * HIGH from the very first instant of boot - see the backfeed comment
 * above) was tried here 2026-09-13 and reverted the same day: a real
 * battery disconnect/reconnect with the card left inserted stopped
 * refreshing correctly with the hook in place, working again as soon as
 * it was removed - the only variable that changed between a verified-
 * good POR test and a broken one. Leading theory, not confirmed: SD
 * cards expect CS held *high* (deselected) while their own VDD ramps up
 * during power-on (part of the standard SPI-mode entry sequence) -
 * forcing it low that early may have left the card selected through its
 * very first real power-up (app_sd_init()'s, well after boot), putting
 * it in a state that then corrupted later bus traffic. The brief
 * CS-high window between spi3's init and the first app_sd_power_on()
 * call (a few tens to low hundreds of ms, no bus traffic happening
 * during it) is apparently harmless in practice - every POR test that
 * relies on just that window plus app_sd_power_off()'s CS-low-before-
 * power-down fix (below) has worked. Don't reintroduce an early forced
 * CS-low without re-testing a real battery power cycle with the card
 * left inserted, specifically.
 */

static FATFS fat_fs;
static struct fs_mount_t mp = {
	.type = FS_FATFS,
	.fs_data = &fat_fs,
	.mnt_point = DISK_MOUNT_PT,
};

static bool sd_mounted;

/*
 * Guarded by display.c's display_bus_lock for its whole lifetime - every
 * call site that touches it (display.c's app_display_lock()/unlock(),
 * and this file's own app_sd_init()/app_sd_deinit()) only ever does so
 * while already holding that lock, so this doesn't need its own separate
 * synchronization.
 */
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
		/*
		 * Force CS low (the GPIO_ACTIVE_LOW "asserted" level) before
		 * cutting power - see the comment on sd_cs_gpio above. Doing
		 * this only once refcount actually reaches 0 (not on every
		 * matching app_sd_power_off() call) means it can't glitch CS
		 * out from under some other holder still using the card.
		 */
		gpio_pin_set_dt(&sd_cs_gpio, 1);
		regulator_disable(sd_regulator);
	}
}

int app_sd_init(void)
{
	int ret;

	app_display_lock();

	/*
	 * This establishes app_sd_init()'s own persistent hold, on top of
	 * whatever nested hold app_display_lock() itself just took - see
	 * the reference-counting note on app_sd_power_on()/
	 * app_sd_power_off() in sd.h. app_display_unlock() below only
	 * releases the nested one; this one survives until app_sd_deinit().
	 */
	ret = app_sd_power_on();
	if (ret) {
		app_display_unlock();
		return ret;
	}

	ret = fs_mount(&mp);
	if (ret) {
		printk("SD: mount failed (%d)\n", ret);
		/*
		 * Leave the power hold in place even though mount failed -
		 * per sd.h, an inserted-but-unpowered card corrupts every
		 * later display refresh, which is worse than paying for the
		 * card's idle current until app_sd_deinit() releases this
		 * hold.
		 */
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

	/* Releases app_sd_init()'s persistent hold (if one was ever
	 * established) - safe to call even if it wasn't.
	 */
	app_sd_power_off();

	app_display_unlock();
}

int app_sd_full_path(const char *path, char *out, size_t out_size)
{
	int ret;

	/*
	 * Deliberately does not mount or touch sd_power_refcount/sd_cs_gpio
	 * itself - see the doc comment in sd.h. Only app_sd_init()/
	 * app_sd_deinit() (and display.c's per-refresh bracket) are allowed
	 * near the power switch/CS pin, so the two confirmed backfeed
	 * mechanisms stay fixed in exactly one place each.
	 */
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
			/* ret != 0: read error; empty name: end of directory. */
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
