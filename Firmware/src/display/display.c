/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dbi.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include "display.h"
#include "sd.h"

static const struct device *const display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static const struct device *const spi3_dev = DEVICE_DT_GET(DT_NODELABEL(spi3));

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

#define SSD1680_CMD_DEEP_SLEEP_MODE 0x10U
#define SSD1680_DEEP_SLEEP_MODE_1   0x01U

static K_MUTEX_DEFINE(display_bus_lock);

static int spi3_pm_refcount;

void app_display_lock(void)
{
	k_mutex_lock(&display_bus_lock, K_FOREVER);

	if (spi3_pm_refcount == 0) {
		pm_device_action_run(spi3_dev, PM_DEVICE_ACTION_RESUME);
	}
	spi3_pm_refcount++;

	(void)app_sd_power_on();
}

void app_display_unlock(void)
{
	app_sd_power_off();

	spi3_pm_refcount--;
	if (spi3_pm_refcount == 0) {
		pm_device_action_run(spi3_dev, PM_DEVICE_ACTION_SUSPEND);
	}

	k_mutex_unlock(&display_bus_lock);
}

static void wait_busy(void)
{
	while (gpio_pin_get_dt(&epd_busy_gpio) > 0) {
		k_msleep(1);
	}
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

	display_blanking_on(display_dev);

	ret = app_display_write(0, 0, width, height, buf, buf_size);
	if (ret) {
		printk("display_write (full) failed: %d\n", ret);
		app_display_unlock();
		return ret;
	}

	ret = display_blanking_off(display_dev);
	wait_busy();

	app_display_unlock();

	return ret;
}

int app_display_refresh_fast(const uint8_t *buf, size_t buf_size)
{
	uint16_t width, height;
	int ret;

	app_display_lock();

	app_display_get_size(&width, &height);

	ret = app_display_write(0, 0, width, height, buf, buf_size);
	if (ret) {
		printk("display_write (fast) failed: %d\n", ret);
	} else {
		wait_busy();
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
	} else {
		wait_busy();
	}

	app_display_unlock();

	return ret;
}

int app_display_deep_sleep(void)
{
	uint8_t data = SSD1680_DEEP_SLEEP_MODE_1;
	int err;

	app_display_lock();

	wait_busy();

	err = mipi_dbi_command_write(epd_mipi_dev, &epd_dbi_cfg,
				      SSD1680_CMD_DEEP_SLEEP_MODE, &data, 1);
	mipi_dbi_release(epd_mipi_dev, &epd_dbi_cfg);

	app_display_unlock();

	return err;
}
