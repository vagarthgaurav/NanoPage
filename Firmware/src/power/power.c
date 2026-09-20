/* SPDX-License-Identifier: Apache-2.0 */

#include "power.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>

#include "config.h"
#include "display.h"
#include "logo_nanopage.h"
#include "rtc.h"
#include "sd.h"

static const struct gpio_dt_spec wake_buttons[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_action), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_next), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_prev), gpios),
};

static struct k_work_delayable idle_work;
static bool woke_from_system_off;
static bool sleep_inhibited;
static void (*idle_hook)(void);

static void idle_work_handler(struct k_work *work);

static void print_and_clear_reset_cause(void)
{
	uint32_t reason = nrf_power_resetreas_get(NRF_POWER);

	woke_from_system_off = (reason & NRF_POWER_RESETREAS_OFF_MASK) != 0;

	printk("power: RESETREAS=0x%08x%s\n", reason,
	       woke_from_system_off ? " (woke from System OFF via GPIO)" : "");

	nrf_power_resetreas_clear(NRF_POWER, reason);
}

void app_power_init(void)
{
	print_and_clear_reset_cause();
	k_work_init_delayable(&idle_work, idle_work_handler);
	k_work_reschedule(&idle_work, K_MSEC(APP_POWER_IDLE_TIMEOUT_MS));
}

void app_power_mark_activity(void)
{
	if (sleep_inhibited) {
		return;
	}

	k_work_reschedule(&idle_work, K_MSEC(APP_POWER_IDLE_TIMEOUT_MS));
}

void app_power_cancel_idle_sync(void)
{
	struct k_work_sync sync;

	k_work_cancel_delayable_sync(&idle_work, &sync);
}

static void show_sleep_message(void)
{
	app_display_refresh_full(app_logo_nanopage, sizeof(app_logo_nanopage));
}

static void enter_system_off(void)
{
	printk("power: %d minutes idle, entering System OFF (wake on any button)\n",
	       APP_POWER_IDLE_TIMEOUT_MS / 60000);

	if (idle_hook) {
		idle_hook();
	}

	show_sleep_message();

	app_rtc_clear_alarm();

	app_display_deep_sleep();

	app_sd_deinit();

	for (size_t i = 0; i < ARRAY_SIZE(wake_buttons); i++) {
		gpio_pin_configure_dt(&wake_buttons[i], GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&wake_buttons[i], GPIO_INT_LEVEL_ACTIVE);
	}

	sys_poweroff();
}

static void idle_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (sleep_inhibited) {
		return;
	}

	enter_system_off();
}

void app_power_set_sleep_inhibited(bool inhibited)
{
	sleep_inhibited = inhibited;

	if (inhibited) {
		k_work_cancel_delayable(&idle_work);
	}
}

void app_power_set_idle_hook(void (*cb)(void))
{
	idle_hook = cb;
}

bool app_power_woke_from_system_off(void)
{
	return woke_from_system_off;
}
