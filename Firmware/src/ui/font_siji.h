/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FONT_SIJI_H_
#define APP_FONT_SIJI_H_

#include <zephyr/display/cfb.h>

/*
 * A hand-picked subset of siji (stark/siji on GitHub, MIT), the same
 * bitmap icon font the old (now-abandoned) Arduino/GxEPD2 firmware used
 * via u8g2 (u8g2_font_siji_t_6x10) for header/footer/file-browser icons -
 * see HeaderBar.cpp/FileBrowser.cpp on the `main` branch. Converted
 * straight from siji's own BDF (its glyphs are native 12x12 there, not
 * the 6x10 u8g2's font name suggests - that name is u8g2's generic size
 * class, not this glyph's real pixel size), bottom-padded to 12x16 for
 * CFB_FONT_MONO_VPACKED's multiple-of-8 height requirement (same
 * constraint as font_tamzen.h), matching the 16px row height already
 * used for Tamzen menu/list text so an icon can sit inline in the same
 * row.
 *
 * siji's own codepoints are scattered Unicode Private Use Area values
 * (e.g. cursor = 0xe09a) - not useful as a `first_char`/`last_char`
 * range for text.c's simple contiguous-array font model. Remapped here
 * to a small contiguous local range instead (0..12, this enum) - these
 * numbers have no meaning outside this font; only the 13 glyphs this
 * project actually uses are kept, not all of siji. The four named icons
 * plus 9 battery fill levels are exactly the set the old firmware used;
 * the real siji codepoints they came from are documented next to each
 * one in font_siji.c.
 */
enum app_text_icon {
	APP_TEXT_ICON_CURSOR = 0,
	APP_TEXT_ICON_FOLDER = 1,
	APP_TEXT_ICON_FILE = 2,
	APP_TEXT_ICON_CHARGING = 3,
	APP_TEXT_ICON_BATTERY_MIN = 4,
	APP_TEXT_ICON_BATTERY_MAX = 12, /* MIN..MAX = 9 fill levels, empty to full */
};

#define APP_TEXT_ICON_BATTERY_LEVELS \
	(APP_TEXT_ICON_BATTERY_MAX - APP_TEXT_ICON_BATTERY_MIN + 1)

extern const struct cfb_font font_siji_icons;

#endif /* APP_FONT_SIJI_H_ */
