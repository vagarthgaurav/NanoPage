/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/sys/util.h>

#include "display.h"

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

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

/* Every refresh call below blocks on the BUSY pin until the panel is
 * physically done, so the wall-clock delta here is the real refresh time -
 * useful for weighing refresh strategy against battery life.
 */
static void log_elapsed(const char *label, int64_t start_ms)
{
	printk("display: %s refresh took %lld ms\n", label, k_uptime_get() - start_ms);
}

int app_display_refresh_full(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int64_t start_ms = k_uptime_get();
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

	ret = display_blanking_off(display_dev);
	log_elapsed("full", start_ms);

	return ret;
}

int app_display_refresh_fast(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int64_t start_ms = k_uptime_get();
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
		return ret;
	}

	log_elapsed("fast", start_ms);

	return ret;
}

int app_display_refresh_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *buf, size_t buf_size)
{
	int64_t start_ms = k_uptime_get();
	int ret = app_display_write(x, y, width, height, buf, buf_size);

	if (ret) {
		printk("display_write (partial) failed: %d\n", ret);
		return ret;
	}

	log_elapsed("partial", start_ms);

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
