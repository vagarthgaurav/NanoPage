/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "charger.h"

static const struct gpio_dt_spec charger_stat_gpio =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charger_status_gpios);

static struct gpio_callback stat_gpio_cb;
static struct k_work stat_change_work;
static void (*stat_change_cb)(void);

static void stat_change_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (stat_change_cb) {
		stat_change_cb();
	}
}

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
