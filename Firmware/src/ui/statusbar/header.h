/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_HEADER_H_
#define APP_HEADER_H_

#include <stdbool.h>

/*
 * Header bar: always grid row 0 (see text.h) of the current text font.
 * Layout, left to right:
 *   - `folder`, left-aligned, truncated to leave room for the clock - OR,
 *     when `bookmarked` is true, a bookmark icon in that exact spot
 *     instead (see app_header_update()'s own doc comment below). The two
 *     are mutually exclusive: reader.c passes an empty `folder` whenever
 *     a book is open (no folder path is meaningful while reading), so
 *     there's nothing for the icon to visually compete with.
 *   - "dd/mm/yy HH:mm" (RTC-sourced), centered.
 *   - charging bolt icon (only while charging) -> percentage -> battery
 *     icon, right-aligned flush to the screen edge.
 * A 1px separator rule is drawn along the bottom of the row (see
 * app_text_draw_hline() in text.c) so the header doesn't need a grid row
 * of its own just for the line.
 *
 * Draws into text.c's framebuffer only - caller still owns pushing it to
 * the panel (app_text_flush() for the cheap partial-refresh case,
 * app_text_full_refresh() for the initial/ghost-clearing draw), same
 * convention as every other text.c-based drawer in this app (see
 * draw_status() in main.c).
 */

#define APP_HEADER_ROW 0

/*
 * Checks the RTC/fuel gauge/charger STAT for anything that would change
 * what the header shows, and only actually redraws the row if something
 * did - a redraw's app_text_flush() means a real SSD1680 partial-refresh
 * SPI burst, not free against the battery-life goal. Meant to be called
 * from the two event sources main.c wires up, not polled on a timer: the
 * RTC's once-a-minute alarm (app_rtc_set_alarm(), fires exactly on the
 * seconds-roll-to-0 edge) and app_charger_set_change_callback() (fires on
 * every STAT transition) - plus directly, from every screen that changes
 * what the header should show right when it happens (browser.c on folder
 * navigation, reader.c on every page turn/bookmark toggle, menu.c/
 * settings.c/reader_menu.c/toc.c/bookmarks.c on their own transitions),
 * same convention throughout.
 *
 * `bookmarked` is only meaningful (and should only ever be true) while
 * reader.c is showing an open book's page - see app_reader_current_page_
 * bookmarked() in reader.h. Every other caller always passes false,
 * which leaves the folder-name area showing `folder` exactly as before
 * this parameter existed.
 *
 * Redraws when: the formatted "dd/mm/yy HH:mm" string changes (i.e. the
 * minute rolled over), the displayed whole-percent battery value changes
 * by at least 1, app_charger_is_charging() flips, `bookmarked` flips, or
 * `folder` itself differs from the last-drawn one. Returns 1 if it
 * redrew, 0 if nothing changed (row left untouched), negative errno if
 * the RTC or fuel gauge read failed (row left untouched either way -
 * unlike the old app_header_draw(), a fuel-gauge failure no longer
 * leaves a stale folder/clock half-drawn, since the change check needs
 * the battery reading too, before it can decide whether to touch the
 * framebuffer at all).
 */
int app_header_update(const char *folder, bool bookmarked);

#endif /* APP_HEADER_H_ */
