/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FONT_BATTERY_H_
#define APP_FONT_BATTERY_H_

#include <zephyr/display/cfb.h>

#define APP_TEXT_BATTERY_WIDTH_PX 18

enum app_text_battery {
	APP_TEXT_BATTERY_CHARGING = 0, /* drawn in place of the level icon while charging */
	APP_TEXT_BATTERY_MIN = 1,
	APP_TEXT_BATTERY_MAX = 5, /* MIN..MAX = 5 fill levels, empty to full */
};

#define APP_TEXT_BATTERY_LEVELS (APP_TEXT_BATTERY_MAX - APP_TEXT_BATTERY_MIN + 1)

extern const struct cfb_font font_battery_icons;

#endif /* APP_FONT_BATTERY_H_ */
