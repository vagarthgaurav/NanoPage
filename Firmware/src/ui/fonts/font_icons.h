/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FONT_ICONS_H_
#define APP_FONT_ICONS_H_

#include <zephyr/display/cfb.h>

enum app_text_icon {
	APP_TEXT_ICON_CURSOR = 0,
	APP_TEXT_ICON_FOLDER = 1,
	APP_TEXT_ICON_FILE = 2,
	APP_TEXT_ICON_SETTINGS = 3,
	APP_TEXT_ICON_BOOKMARK = 4,
	APP_TEXT_ICON_CALENDAR = 5,
	APP_TEXT_ICON_BOOK = 6,
};

extern const struct cfb_font font_icons;

#endif /* APP_FONT_ICONS_H_ */
