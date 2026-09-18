/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_READER_MENU_H_
#define APP_READER_MENU_H_

#include <stdbool.h>

/*
 * In-book menu, opened by holding the Action button while reading (see
 * buttons.h's app_buttons_set_long_press_callback() and reader.c's
 * open_reader_menu()) - a structural sibling of menu.c's app-launcher
 * screen, but scoped to the currently-open book. A fixed list, same
 * icon+name row convention as menu.c/browser.c:
 *
 *   - "Back to Book"       - returns to reading, position unchanged.
 *   - "Table of Contents"  - opens toc.c's chapter list (see toc.h).
 *   - "Bookmarks"          - reserved for future explicit-bookmark-slot
 *                            work (docs/book-format.md's NPB_MAX_BOOKMARKS
 *                            sidecar field); currently a no-op selection.
 *
 * Prev/Next move the cursor, Action launches the selected entry - same
 * as every other list screen in this app. Deliberately doesn't try to
 * remember its own place across visits: every call is a fresh, small,
 * three-entry list.
 */

/*
 * Shows the menu and takes over button input. `on_exit` is both this
 * menu's own "Back to Book" handler and the one it hands straight
 * through to toc.c (see toc.h) - selecting a chapter there returns
 * directly to reading, not back up through this menu, so there's only
 * ever one exit path regardless of how the user got here.
 */
void app_reader_menu_show(void (*on_exit)(void));

/* For main.c's header-label priority chain (see app_menu_is_active()/
 * app_settings_is_active() for the identical pattern) - true while this
 * menu (not toc.c's own screen) currently owns button input.
 */
bool app_reader_menu_is_active(void);

#endif /* APP_READER_MENU_H_ */
