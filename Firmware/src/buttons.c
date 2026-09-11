/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>
#include <zephyr/input/input.h>

#include "power.h"

static void button_event_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	printk("button: code=%u %s\n", evt->code, evt->value ? "pressed" : "released");

	if (evt->value) {
		app_power_mark_activity();
	}
}
INPUT_CALLBACK_DEFINE(NULL, button_event_cb, NULL);
