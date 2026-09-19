/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/pm/device_runtime.h>

#include "buttons.h"
#include "power.h"

static const struct device *const buttons_dev = DEVICE_DT_GET(DT_PATH(buttons));

static const struct device *const longpress_dev = DEVICE_DT_GET(DT_NODELABEL(action_longpress));

static void (*press_cb)(enum app_button button);
static void (*long_press_cb)(void);
static void (*combo_cb)(void);

static bool prev_held;
static bool action_held;
static bool suppress_prev_release;
static bool suppress_action_echo;

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

	printk("button: dev=%s code=%u %s\n", evt->dev ? evt->dev->name : "?", evt->code,
	       evt->value ? "pressed" : "released");

	if (evt->value) {
		app_power_mark_activity();
	}

	switch (evt->code) {
	case INPUT_KEY_LEFT:
		if (!combo_cb) {
			if (evt->value && press_cb) {
				press_cb(APP_BUTTON_PREV);
			}
			break;
		}

		if (evt->value) {
			prev_held = true;
			if (action_held) {
				suppress_prev_release = true;
				suppress_action_echo = true;
				combo_cb();
			}
		} else {
			prev_held = false;
			if (suppress_prev_release) {
				suppress_prev_release = false;
			} else if (press_cb) {
				press_cb(APP_BUTTON_PREV);
			}
		}
		break;
	case INPUT_KEY_RIGHT:
		if (evt->value && press_cb) {
			press_cb(APP_BUTTON_NEXT);
		}
		break;
	case INPUT_KEY_ENTER:
		if (evt->dev == buttons_dev) {
			if (combo_cb) {
				if (evt->value) {
					action_held = true;
					if (prev_held) {
						suppress_prev_release = true;
						suppress_action_echo = true;
						combo_cb();
					}
				} else {
					action_held = false;
				}
			}
			break;
		}

		if (evt->dev == longpress_dev && evt->value) {
			if (suppress_action_echo) {
				suppress_action_echo = false;
			} else if (press_cb) {
				press_cb(APP_BUTTON_ACTION);
			}
		}
		break;
	case INPUT_KEY_MENU:
		if (evt->dev == longpress_dev && evt->value) {
			if (suppress_action_echo) {
				suppress_action_echo = false;
			} else if (long_press_cb) {
				long_press_cb();
			}
		}
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

void app_buttons_set_long_press_callback(void (*cb)(void))
{
	long_press_cb = cb;
}

void app_buttons_set_combo_callback(void (*cb)(void))
{
	combo_cb = cb;

	prev_held = false;
	action_held = false;
	suppress_prev_release = false;
	suppress_action_echo = false;
}
