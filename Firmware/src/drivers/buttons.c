/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device_runtime.h>

#include "buttons.h"
#include "power.h"

/* The one gpio-keys device instance covering all three buttons
 * (button_action/button_next/button_prev are its per-key devicetree
 * children, not separate device instances) - see app_buttons_init()'s
 * doc comment in buttons.h for why this needs a permanent runtime-PM
 * reference.
 */
static const struct device *const buttons_dev = DEVICE_DT_GET(DT_PATH(buttons));

static void (*press_cb)(enum app_button button);

int app_buttons_init(void)
{
	int ret;

	if (!device_is_ready(buttons_dev)) {
		printk("buttons: device not ready\n");
		return -ENODEV;
	}

	ret = pm_device_runtime_get(buttons_dev);
	if (ret) {
		printk("buttons: pm_device_runtime_get failed (%d)\n", ret);
		return ret;
	}

	return 0;
}

static void button_event_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	printk("button: code=%u %s\n", evt->code, evt->value ? "pressed" : "released");

	if (!evt->value) {
		return;
	}

	app_power_mark_activity();

	if (!press_cb) {
		return;
	}

	switch (evt->code) {
	case INPUT_KEY_LEFT:
		press_cb(APP_BUTTON_PREV);
		break;
	case INPUT_KEY_RIGHT:
		press_cb(APP_BUTTON_NEXT);
		break;
	case INPUT_KEY_ENTER:
		press_cb(APP_BUTTON_ACTION);
		break;
	default:
		break;
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_event_cb, NULL);

void app_buttons_set_press_callback(void (*cb)(enum app_button button))
{
	press_cb = cb;
}
