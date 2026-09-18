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
 *   - "Bookmarks"          - opens bookmarks.c's bookmark list (see
 *                            bookmarks.h) - the explicit bookmarks added/
 *                            removed via reader.c's Prev+Action combo.
 *
 * Prev/Next move the cursor, Action launches the selected entry - same
 * as every other list screen in this app. Deliberately doesn't try to
 * remember its own place across visits: every call is a fresh, small,
 * three-entry list.
 */

/*
 * Shows the menu and takes over button input. `on_exit` is this menu's
 * own "Back to Book" handler, and also what toc.c/bookmarks.c call
 * (as their `on_selected`, see toc.h/bookmarks.h) when the user actually
 * picks a chapter or bookmark there - either way, reading resumes
 * directly. Their own "Back" entry instead returns to *this* menu
 * (reader_menu.c's own return_to_menu(), passed as their `on_back`), not
 * all the way past it to the book - so backing out of the chapter/
 * bookmark list lands you back here, where you can still switch to the
 * other one or back out further to the book.
 */
void app_reader_menu_show(void (*on_exit)(void));

/* For main.c's header-label priority chain (see app_menu_is_active()/
 * app_settings_is_active() for the identical pattern) - true while this
 * menu (not toc.c's own screen) currently owns button input.
 */
bool app_reader_menu_is_active(void);

#endif /* APP_READER_MENU_H_ */
