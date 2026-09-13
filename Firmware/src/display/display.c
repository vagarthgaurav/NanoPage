/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/sys/util.h>

#include "display.h"

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
		return ret;
	}

	return display_blanking_off(display_dev);
}

int app_display_refresh_fast(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int ret;

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

	return ret;
}

int app_display_refresh_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *buf, size_t buf_size)
{
	int ret = app_display_write(x, y, width, height, buf, buf_size);

	if (ret) {
		printk("display_write (partial) failed: %d\n", ret);
	}

	return ret;
}

int app_display_clear(bool black)
{
	uint16_t width, height;

	app_display_get_size(&width, &height);

	/*
	 * The ssd16xx driver writes this buffer straight to the SSD1680's
	 * WRITE_RAM command with no inversion, so despite Zephyr's
	 * PIXEL_FORMAT_MONO10 doc comment (1=black), what actually reaches
	 * the panel is the datasheet's native RAM convention: bit=1 ->
	 * white, bit=0 -> black (SSD1680 datasheet, "Write RAM (Black
	 * White)" 0x24h command description).
	 */
	/* See the buffer-format comment in display.h: vtiled, width bytes
	 * per 8-row band. height is guaranteed a multiple of 8 (checked by
	 * ssd16xx_get_capabilities()), so this is exact, no rounding.
	 */
	size_t buf_size = (size_t)width * (height / 8);
	uint8_t *buf = k_malloc(buf_size);

	if (!buf) {
		return -ENOMEM;
	}

	memset(buf, black ? 0x00 : 0xFF, buf_size);

	int ret = app_display_refresh_full(buf, buf_size);

	k_free(buf);

	return ret;
}

int app_display_deep_sleep(void)
{
	uint8_t data = SSD1680_DEEP_SLEEP_MODE_2;
	int err;

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

	return err;
}
