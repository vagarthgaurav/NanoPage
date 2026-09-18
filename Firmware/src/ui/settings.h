/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdbool.h>

/*
 * Settings screen - a structural sibling of browser.c/reader.c, launched
 * from menu.h. Renders into the same content area (between the header and
 * footer rows) those use. Only one setting exists so far: the font book
 * content renders in (reader.h's app_reader_get_font()/app_reader_set_font(),
 * documented there as "a future settings menu's hook" - this is that menu).
 * Prev/Next cycle the setting's value (applied immediately); Action leaves
 * the screen. With only one setting and three buttons total, there's no
 * spare button for moving between multiple settings yet - revisit the
 * layout if a second setting is ever added.
 */

/*
 * Draws the screen and takes over button input. `on_exit` is called once,
 * from the Action-button handler - same on_exit convention as
 * app_reader_open()/app_browser_init(). Always succeeds (no SD/hardware
 * access involved).
 */
void app_settings_init(void (*on_exit)(void));

/* Whether the settings screen is the one currently driving button input -
 * for main.c's header-label dispatch (see app_menu_is_active() for the
 * identical reasoning).
 */
bool app_settings_is_active(void);

#endif /* APP_SETTINGS_H_ */
