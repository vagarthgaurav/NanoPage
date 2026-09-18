/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_MENU_H_
#define APP_MENU_H_

#include <stdbool.h>

/*
 * App menu - the screen the device boots into (main.c), replacing the old
 * "boot straight into the file browser" behavior. A short, fixed list of
 * apps (currently: File Browser, Settings), rendered into text.c's grid the
 * same way browser.c renders a directory listing - one entry per row, a
 * cursor icon on the selected one, between the header (row 0) and footer
 * (the grid's last row). Prev/Next move the cursor; Action launches the
 * selected app.
 *
 * Each app is responsible for handing control back here when it's done -
 * see app_browser_init()'s `on_exit` (called from the SD root's synthetic
 * ".." entry) and app_settings_init()'s `on_exit` (called from its own
 * Action/"back"), both of which this module passes app_menu_show() as. This
 * mirrors reader.h's on_exit convention, just one layer up: browser.c is
 * both a caller of that convention (reader.c) and, from the menu's point of
 * view, a callee of it too.
 */

/*
 * Draws the menu and takes over button input. Doesn't touch the SD card or
 * any other peripheral - safe to call at boot before anything else is
 * mounted, and cheap to call again as an on_exit callback from whatever app
 * was launched (the selection cursor is left wherever it was, so returning
 * from an app re-shows the same highlighted entry rather than resetting to
 * the top).
 */
void app_menu_show(void);

/* Whether the menu is the one currently driving button input - for main.c's
 * header-label dispatch (on_header_event() in main.c already has to pick
 * between several mutually-exclusive "what's on screen right now" sources -
 * USB connected, this, the settings screen, or the browser's current path -
 * this is this module's contribution to that chain).
 */
bool app_menu_is_active(void);

#endif /* APP_MENU_H_ */
