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
static void (*combo_cb)(void);

/*
 * Raw hold-state for Prev/Action, tracked only while combo_cb is
 * registered - see app_buttons_set_combo_callback()'s doc comment for
 * the detection/suppression scheme these drive. suppress_prev_release/
 * suppress_action_echo are one-shot: set together the instant a combo
 * fires (at that point both buttons are, by construction, still held -
 * detection only ever happens on a press while the other is down - so
 * exactly one future Prev-release and one future Action release/echo
 * are guaranteed to follow), and each consumed independently the next
 * time its own button's normal dispatch point is reached, rather than a
 * persistent "combo in progress" flag that would need careful reset
 * timing against the longpress device's own delayed, asynchronously
 * re-queued echo event.
 */
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
			/* Raw, instant - not wrapped by the longpress
			 * device, and no combo tracking to do.
			 */
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
			/* Raw press/release - only used for combo hold
			 * tracking. The actual Action dispatch always comes
			 * from the longpress device's echo below, same as
			 * before combo support existed.
			 */
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

		/*
		 * The longpress device's short-press echo (on release,
		 * once it's confirmed this wasn't held long enough to be a
		 * long press). See buttons.h's doc comment on
		 * app_buttons_set_press_callback() for the user-visible
		 * effect (Action dispatches on release, not press).
		 */
		if (evt->dev == longpress_dev && evt->value) {
			if (suppress_action_echo) {
				suppress_action_echo = false;
			} else if (press_cb) {
				press_cb(APP_BUTTON_ACTION);
			}
		}
		break;
	case INPUT_KEY_MENU:
		/* Only ever comes from the longpress device - no raw
		 * source emits this code. Also gated on suppress_action_echo
		 * (not just checked, but consumed) since a combo can in
		 * principle fire while Action is held past the long-press
		 * threshold - short/long echoes are mutually exclusive per
		 * hold cycle, so at most one of these two branches ever
		 * actually consumes the flag for a given press.
		 */
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

	/* Fresh start whenever combo tracking is (re)armed or torn down -
	 * a stale held/suppress flag left over from whatever was happening
	 * right before this call (e.g. reader_menu.c taking over mid-hold)
	 * must never leak into the next screen's button handling.
	 */
	prev_held = false;
	action_held = false;
	suppress_prev_release = false;
	suppress_action_echo = false;
}
