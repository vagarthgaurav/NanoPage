/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/sys/util.h>

#include "display.h"
#include "sd.h"

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/*
 * Talks to the panel directly over MIPI DBI for app_display_deep_sleep() -
 * see the long comment on that function in display.h for why this bypasses
 * the Zephyr display API. mipi_dev/dbi_cfg are built to exactly match what
 * the ssd16xx driver itself constructs internally (SSD16XX_DEFINE() in
 * ssd16xx.c: .mode = MIPI_DBI_MODE_SPI_4WIRE, same SPI_OP_MODE_MASTER |
 * SPI_WORD_SET(8) | SPI_HOLD_ON_CS | SPI_LOCK_ON operation flags) - this
 * isn't a separate/competing bus config, just the same one, independently
 * expressed since the driver's copy is private to its own .c file.
 */
static const struct device *const epd_mipi_dev =
	DEVICE_DT_GET(DT_PARENT(DT_CHOSEN(zephyr_display)));
static const struct gpio_dt_spec epd_busy_gpio =
	GPIO_DT_SPEC_GET(DT_CHOSEN(zephyr_display), busy_gpios);
static const struct mipi_dbi_config epd_dbi_cfg = {
	.mode = MIPI_DBI_MODE_SPI_4WIRE,
	.config = MIPI_DBI_SPI_CONFIG_DT(DT_CHOSEN(zephyr_display),
					  SPI_OP_MODE_MASTER | SPI_WORD_SET(8) |
					  SPI_HOLD_ON_CS | SPI_LOCK_ON, 0),
};

/* SSD1680 datasheet, Table 7-1 "Command Table": command 0x10, data A[1:0].
 * 00 = Normal mode (POR), 01 = Deep Sleep Mode 1 (retains RAM, ~1uA typ),
 * 11 = Deep Sleep Mode 2 (RAM not retained, ~0.7uA typ). Mode 2 is strictly
 * better for us: nothing here ever relies on Deep Sleep retaining RAM,
 * since waking always means a full reboot and a from-scratch re-init
 * anyway (see display.h).
 */
#define SSD1680_CMD_DEEP_SLEEP_MODE 0x10U
#define SSD1680_DEEP_SLEEP_MODE_2   0x03U

/* See the comment on app_display_lock()/app_display_unlock() in display.h. */
static K_MUTEX_DEFINE(display_bus_lock);

void app_display_lock(void)
{
	k_mutex_lock(&display_bus_lock, K_FOREVER);

	/*
	 * Power the microSD card for the duration of whatever bus activity
	 * the caller is about to do, per the hardware constraint documented
	 * in sd.h - best-effort: a failure here (e.g. the regulator device
	 * genuinely not ready) doesn't block the refresh itself, just logs
	 * from within app_sd_power_on().
	 *
	 * This is NOT redundant with app_sd_power_off()'s CS-low fix (see
	 * sd.c) - that fix only stops the *static* backfeed present while
	 * everything on spi3 sits idle. During an actual refresh, SCK/MOSI
	 * are being actively driven regardless of CS, and those shared
	 * lines forward-bias the unpowered card's own ESD diodes just the
	 * same - confirmed 2026-09-13 by removing this bracket and finding
	 * refreshes broke again even with the CS fix and a boot-time CS
	 * hook both in place. With no bus-isolation switch on this board,
	 * actually powering the card for the refresh's duration is the only
	 * fix available in firmware.
	 */
	(void)app_sd_power_on();
}

void app_display_unlock(void)
{
	app_sd_power_off();

	k_mutex_unlock(&display_bus_lock);
}

int app_display_init(void)
{
	if (!device_is_ready(display_dev)) {
		printk("Display device not ready\n");
		return -ENODEV;
	}

	struct display_capabilities caps;

	display_get_capabilities(display_dev, &caps);
	printk("display: %ux%u, pixel format 0x%02x\n", caps.x_resolution, caps.y_resolution,
	       caps.current_pixel_format);

	return 0;
}

void app_display_get_size(uint16_t *width, uint16_t *height)
{
	struct display_capabilities caps;

	display_get_capabilities(display_dev, &caps);
	*width = caps.x_resolution;
	*height = caps.y_resolution;
}

enum display_orientation app_display_get_orientation(void)
{
	struct display_capabilities caps;

	display_get_capabilities(display_dev, &caps);
	return caps.current_orientation;
}

static int app_display_write(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
			      const uint8_t *buf, size_t buf_size)
{
	struct display_buffer_descriptor desc = {
		.buf_size = buf_size,
		.width = width,
		.height = height,
		.pitch = width,
	};

	return display_write(display_dev, x, y, &desc, buf);
}

int app_display_refresh_full(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int ret;

	app_display_lock();

	app_display_get_size(&width, &height);

	/*
	 * blanking_on() switches the active waveform to FULL (even if a
	 * "partial" devicetree profile made fast/partial the default) and
	 * makes the write below only load RAM without triggering a
	 * refresh; blanking_off() then triggers the actual full update.
	 */
	display_blanking_on(display_dev);

	ret = app_display_write(0, 0, width, height, buf, buf_size);
	if (ret) {
		printk("display_write (full) failed: %d\n", ret);
		app_display_unlock();
		return ret;
	}

	ret = display_blanking_off(display_dev);

	app_display_unlock();

	return ret;
}

int app_display_refresh_fast(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int ret;

	app_display_lock();

	app_display_get_size(&width, &height);

	/*
	 * With blanking already off, a plain write() picks the "partial"
	 * profile (SSD1680 OTP Mode 2 waveform) automatically and triggers
	 * the refresh immediately - same waveform as
	 * app_display_refresh_partial(), just covering the whole screen.
	 */
	ret = app_display_write(0, 0, width, height, buf, buf_size);
	if (ret) {
		printk("display_write (fast) failed: %d\n", ret);
	}

	app_display_unlock();

	return ret;
}

int app_display_refresh_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *buf, size_t buf_size)
{
	app_display_lock();

	int ret = app_display_write(x, y, width, height, buf, buf_size);

	if (ret) {
		printk("display_write (partial) failed: %d\n", ret);
	}

	app_display_unlock();

	return ret;
}

int app_display_deep_sleep(void)
{
	uint8_t data = SSD1680_DEEP_SLEEP_MODE_2;
	int err;

	app_display_lock();

	/* Mirrors the driver's own ssd16xx_busy_wait() convention: don't
	 * issue a new command while the panel is still finishing the
	 * previous one. In practice this is already guaranteed by the
	 * refresh functions above blocking until BUSY clears, but that's
	 * an invariant of *this* app's call order, not of this function -
	 * keep the check.
	 */
	while (gpio_pin_get_dt(&epd_busy_gpio) > 0) {
		k_msleep(1);
	}

	err = mipi_dbi_command_write(epd_mipi_dev, &epd_dbi_cfg,
				      SSD1680_CMD_DEEP_SLEEP_MODE, &data, 1);
	mipi_dbi_release(epd_mipi_dev, &epd_dbi_cfg);

	app_display_unlock();

	return err;
}
