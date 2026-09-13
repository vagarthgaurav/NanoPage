/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "charger.h"

/*
 * BQ25170 STAT pin (P0.03) - open-drain, pulled up externally (R8, 10k to
 * +3V3 per the schematic), no internal pull configured here. Per the
 * datasheet (docs/bq25170.pdf, Table 5-1 "Pin Functions"): LOW = charge in
 * progress, HIGH = charge complete or charge disabled. On a fault
 * condition the pin instead blinks at 1Hz - a plain level read during a
 * fault will alternate between "charging"/"not charging" roughly every
 * 500ms rather than reporting a distinct fault state. Not handled
 * specially (yet) - app_charger_is_charging() just reports whatever level
 * STAT happens to be at the moment it's called.
 */
static const struct gpio_dt_spec charger_stat_gpio =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charger_status_gpios);

static struct gpio_callback stat_gpio_cb;
static struct k_work stat_change_work;
static void (*stat_change_cb)(void);

/* Deferred handler - runs on the system workqueue, safe to do the kind of
 * work app_charger_set_change_callback() callers actually want (redrawing
 * the header, which means I2C reads and an SPI display refresh), unlike
 * the raw GPIO ISR below.
 */
static void stat_change_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (stat_change_cb) {
		stat_change_cb();
	}
}

/* Runs in GPIOTE ISR context (same mechanism buttons.c's gpio-keys/input
 * subsystem relies on under the hood) - kept to the bare minimum of
 * submitting the deferred work above, no I2C/display calls here.
 */
static void stat_gpio_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&stat_change_work);
}

int app_charger_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&charger_stat_gpio)) {
		printk("charger: STAT gpio not ready\n");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&charger_stat_gpio, GPIO_INPUT);
	if (ret) {
		return ret;
	}

	k_work_init(&stat_change_work, stat_change_work_handler);

	gpio_init_callback(&stat_gpio_cb, stat_gpio_isr, BIT(charger_stat_gpio.pin));
	ret = gpio_add_callback(charger_stat_gpio.port, &stat_gpio_cb);
	if (ret) {
		return ret;
	}

	/*
	 * Both edges: STAT toggling either direction (charge starts, ends, or
	 * completes) is a real event worth reacting to, not just the
	 * low-going "started charging" edge.
	 */
	return gpio_pin_interrupt_configure_dt(&charger_stat_gpio, GPIO_INT_EDGE_BOTH);
}

bool app_charger_is_charging(void)
{
	return gpio_pin_get_dt(&charger_stat_gpio) > 0;
}

void app_charger_set_change_callback(void (*cb)(void))
{
	stat_change_cb = cb;
}
