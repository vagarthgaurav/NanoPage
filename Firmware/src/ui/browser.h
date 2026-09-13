/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BROWSER_H_
#define APP_BROWSER_H_

/*
 * File/folder browser UI. Renders the SD card's directory listing into
 * text.c's grid, between the header (row 0, header.h) and footer (the
 * grid's last row, footer.h) - one entry per row, a folder/file siji icon
 * next to each name, a cursor icon on the selected row. Registers itself
 * as buttons.h's press callback: Prev/Next move the selection cursor,
 * Action opens the selected folder (or goes up a level for the synthetic
 * ".." entry shown whenever the current directory isn't the SD root), or -
 * for a plain file whose name ends in ".npb" - hands off to reader.h's
 * book reader (which takes over button input until the book is closed,
 * see reader.h). Selecting any other plain file does nothing.
 *
 * Refresh policy:
 *   - A cursor move that stays on the current page redraws only the two
 *     affected rows (cursor icon erase + redraw) - text.c's app_text_
 *     flush() coalesces adjacent dirty rows into one combined SSD1680
 *     partial-refresh transaction rather than one per row (see the
 *     comment on app_text_flush() in text.c), which matters here since
 *     these two rows are always exactly one apart: without it, each
 *     cursor move paid two separate ~530ms partial-refresh waits, visibly
 *     one row at a time and in ascending-row order regardless of which
 *     direction the cursor actually moved (so Prev looked like it drew
 *     the new position before erasing the old one).
 *   - Moving past the edge of the page (scrolling) redraws every visible
 *     content row - also coalesced into one combined refresh - through
 *     app_text_flush(), except every APP_BROWSER_FULL_REFRESH_EVERY-th
 *     one of those, which goes through app_text_full_refresh() instead to
 *     clear the ghosting partial refreshes build up over time. Same
 *     policy the abandoned Arduino-era FileBrowser used (Config::
 *     FULL_REFRESH_EVERY, see git history commit 3fb05c8) - scoped to
 *     this module rather than a display-wide counter, matching how
 *     header.c/footer.c already each own their own refresh decisions.
 *   - Opening or leaving a folder (Action) always goes through
 *     app_text_full_refresh() directly, never counted against the above -
 *     a deliberate, infrequent navigation action, unlike cursor moves/
 *     scrolls which happen constantly, so it's worth the flashing full
 *     redraw to keep an entirely new listing ghost-free, and it resets
 *     the partial counter for the run of moves that follows.
 */

/*
 * Mounts the SD card (sd.h's app_sd_init() - left mounted for as long as
 * the browser is active, not just for this one call, so navigating
 * between folders doesn't re-pay a power-up/remount every time) and lists
 * `root` (e.g. "/" for the SD card's own root) as the first page. Also
 * registers itself via app_buttons_set_press_callback() - once this
 * returns 0, button presses drive the browser directly; don't also
 * install a separate button callback while it's active.
 *
 * Returns 0 on success, negative errno if the card couldn't be mounted
 * or `root` couldn't be listed (the card is left mounted either way, same
 * as app_sd_init() itself - see sd.h).
 */
int app_browser_init(const char *root);

/*
 * The browser's current directory, SD-root-relative and always starting
 * with '/' (e.g. "/books"). For main.c/header.c to display alongside the
 * clock/battery - see the header.h TODO this closes. Valid only after a
 * successful app_browser_init(); the returned pointer is owned by
 * browser.c and only valid until the next navigation.
 */
const char *app_browser_get_current_path(void);

#endif /* APP_BROWSER_H_ */
