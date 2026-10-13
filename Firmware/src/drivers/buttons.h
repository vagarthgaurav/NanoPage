/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BUTTONS_H_
#define APP_BUTTONS_H_

enum app_button {
	APP_BUTTON_PREV,
	APP_BUTTON_NEXT,
	APP_BUTTON_ACTION,
};

int app_buttons_init(void);

void app_buttons_set_press_callback(void (*cb)(enum app_button button));

void app_buttons_set_long_press_callback(void (*cb)(void));

void app_buttons_set_combo_callback(void (*cb)(void));

/*
 * While a hold callback is set, PREV/NEXT taps are delivered through the
 * press callback on RELEASE (so a hold does not also trigger a tap) and
 * holds (>= 500 ms, fired at the timeout while still held) go to the hold
 * callback. Pass NULL to restore instant press-on-press behaviour. Not
 * supported together with the combo callback (reader only).
 */
void app_buttons_set_hold_callback(void (*cb)(enum app_button button));

#endif /* APP_BUTTONS_H_ */
