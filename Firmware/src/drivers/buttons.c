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

/* nanopage.dts's longpress pseudo-device - the sole source of
 * APP_BUTTON_ACTION dispatch (see button_event_cb() below for why the
 * raw buttons_dev INPUT_KEY_ENTER event is ignored instead).
 */
static const struct device *const longpress_dev = DEVICE_DT_GET(DT_NODELABEL(action_longpress));

static void (*press_cb)(enum app_button button);
static void (*long_press_cb)(void);

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

	if (!evt->value) {
		return;
	}

	app_power_mark_activity();

	switch (evt->code) {
	case INPUT_KEY_LEFT:
		/* Raw, instant - not wrapped by the longpress device. */
		if (press_cb) {
			press_cb(APP_BUTTON_PREV);
		}
		break;
	case INPUT_KEY_RIGHT:
		if (press_cb) {
			press_cb(APP_BUTTON_NEXT);
		}
		break;
	case INPUT_KEY_ENTER:
		/*
		 * Two sources emit this code: the raw buttons_dev press
		 * (the instant the user's finger lands) and the longpress
		 * device's short-press echo (on release, once it's
		 * confirmed this wasn't held long enough to be a long
		 * press). Only the latter should ever dispatch - reacting
		 * to the raw press too would fire APP_BUTTON_ACTION twice
		 * per short tap. See buttons.h's doc comment on
		 * app_buttons_set_press_callback() for the user-visible
		 * effect (Action dispatches on release, not press).
		 */
		if (evt->dev == longpress_dev && press_cb) {
			press_cb(APP_BUTTON_ACTION);
		}
		break;
	case INPUT_KEY_MENU:
		/* Only ever comes from the longpress device - no raw
		 * source emits this code.
		 */
		if (long_press_cb) {
			long_press_cb();
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
