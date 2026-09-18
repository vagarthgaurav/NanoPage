/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BUTTONS_H_
#define APP_BUTTONS_H_

/*
 * Physical button identifiers, decoupled from the raw INPUT_KEY_* codes
 * (button_prev/button_next/button_action in nanopage.dts) so callers don't
 * need to know which key code maps to which physical button.
 */
enum app_button {
	APP_BUTTON_PREV,
	APP_BUTTON_NEXT,
	APP_BUTTON_ACTION,
};

/*
 * Takes a permanent reference on the gpio-keys "buttons" device via
 * pm_device_runtime_get() so it never idles into PM_DEVICE_ACTION_SUSPEND.
 *
 * This matters more than it looks: gpio_keys_init() (Zephyr's in-tree
 * input_gpio_keys.c) unconditionally calls pm_device_runtime_enable() on
 * itself at the end of its own init - and per that API's own documented
 * behavior, enabling runtime PM on a device already in the ACTIVE state
 * with a zero usage count suspends it immediately. Nothing else in Zephyr
 * ever takes a reference on a plain input device the way a bus driver's
 * transactions do on SPIM3/TWIM1 (this project's actual, intended target
 * for CONFIG_PM_DEVICE_RUNTIME - see prj.conf), so without this call the
 * buttons device self-suspends moments after boot and stays that way
 * forever: gpio_keys_pm_action()'s SUSPEND case disables each button's
 * GPIO interrupt and reconfigures the pin to GPIO_DISCONNECTED, so a
 * physical press generates no input event at all - confirmed on hardware
 * via a live PIN_CNF register read (0x00000002 = input, buffer
 * disconnected, no pull, exactly gpio_keys_pm_action()'s SUSPEND state)
 * on all three button pins, weeks after CONFIG_PM_DEVICE_RUNTIME was
 * first added for the SPIM3/TWIM1 win - this had been silently broken
 * since then, just never re-tested until browser.c needed real button
 * input. power.c's own System-OFF wake arming is unaffected by any of
 * this - it drives the same physical pins directly via the raw GPIO API,
 * bypassing gpio-keys/this device entirely, so it always reconfigures
 * them itself right before sys_poweroff() regardless of gpio-keys' state.
 *
 * Call once, after app_display_init() and before relying on button
 * presses (main.c calls this early, alongside the other peripheral
 * inits). Returns 0 on success, negative errno if the device isn't
 * ready or the runtime-PM get itself fails.
 */
int app_buttons_init(void);

/*
 * Registers `cb` to be called on every button *press* (not release).
 * Runs on Zephyr's input subsystem thread (CONFIG_INPUT_MODE_THREAD,
 * the default input-processing mode - see CONFIG_INPUT_THREAD_STACK_SIZE
 * in prj.conf), not the system workqueue: gpio-keys debounces on its own
 * k_work_delayable, then posts the debounced event through
 * input_report_key(), which this thread dequeues and dispatches to every
 * INPUT_CALLBACK_DEFINE listener - including this one - synchronously.
 * It's safe to do real work (text.c/display.c calls, as browser.c does)
 * directly from the callback, same as any other thread context. Only one
 * callback is kept at a time; a later call replaces the earlier one. Pass
 * NULL to stop being notified. app_power_mark_activity() is called for
 * every press regardless of whether a callback is registered, so the
 * idle timer keeps working even with none set.
 *
 * APP_BUTTON_ACTION here is sourced from nanopage.dts's `longpress`
 * pseudo-device's short-press echo, not the raw gpio-keys press - see
 * app_buttons_set_long_press_callback() below for why, and buttons.c for
 * the device-filtering that makes this transparent to every caller. The
 * practical effect: Action now dispatches on release rather than on
 * press (unavoidable - short vs. long can't be known until then), a few
 * hundred ms later than before for a normal tap. Prev/Next are
 * unaffected - still raw, instant, press-triggered gpio-keys events.
 */
void app_buttons_set_press_callback(void (*cb)(enum app_button button));

/*
 * Registers `cb` to be called when the Action button is held for at
 * least the `long-delay-ms` nanopage.dts's `longpress` node specifies
 * (currently 500ms) - reader.c's hook for opening the in-book menu
 * (reader_menu.h) without a fourth physical button. Only Action has a
 * long-press variant; Prev/Next don't. Same single-slot/NULL-to-clear
 * convention as app_buttons_set_press_callback() above, but tracked
 * separately - a screen can register a press callback without a
 * long-press one (every screen except reader.c's main reading view
 * leaves this NULL, so a long-press does nothing there).
 */
void app_buttons_set_long_press_callback(void (*cb)(void));

#endif /* APP_BUTTONS_H_ */
