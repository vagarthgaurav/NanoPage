/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FONT_SIJI_H_
#define APP_FONT_SIJI_H_

#include <zephyr/display/cfb.h>

enum app_text_icon {
	APP_TEXT_ICON_CURSOR = 0,
	APP_TEXT_ICON_FOLDER = 1,
	APP_TEXT_ICON_FILE = 2,
	APP_TEXT_ICON_CHARGING = 3,
	APP_TEXT_ICON_BATTERY_MIN = 4,
	APP_TEXT_ICON_BATTERY_MAX = 12, /* MIN..MAX = 9 fill levels, empty to full */
	APP_TEXT_ICON_SETTINGS = 13,
	APP_TEXT_ICON_BOOKMARK = 14,
	APP_TEXT_ICON_CALENDAR = 15,
};

#define APP_TEXT_ICON_BATTERY_LEVELS \
	(APP_TEXT_ICON_BATTERY_MAX - APP_TEXT_ICON_BATTERY_MIN + 1)

extern const struct cfb_font font_siji_icons;

#endif /* APP_FONT_SIJI_H_ */
