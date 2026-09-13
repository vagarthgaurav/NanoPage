/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_HEADER_H_
#define APP_HEADER_H_

/*
 * Header bar: always grid row 0 (see text.h) of the current text font.
 * Layout, left to right:
 *   - `folder`, left-aligned, truncated to leave room for the clock.
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
 * every STAT transition). Both land on the system workqueue, so calling
 * this from either is safe without extra locking - see main.c.
 * Redraws when: the formatted "dd/mm/yy HH:mm" string changes (i.e. the
 * minute rolled over), the displayed whole-percent battery value changes
 * by at least 1, or app_charger_is_charging() flips. `folder` is not
 * currently part of that comparison - fine while it's a compile-time
 * placeholder, but whoever makes it dynamic (a real file browser) needs
 * to fold it into the change check too.
 * Returns 1 if it redrew, 0 if nothing changed (row left untouched),
 * negative errno if the RTC or fuel gauge read failed (row left
 * untouched either way - unlike the old app_header_draw(), a fuel-gauge
 * failure no longer leaves a stale folder/clock half-drawn, since the
 * change check needs the battery reading too, before it can decide
 * whether to touch the framebuffer at all).
 */
int app_header_update(const char *folder);

#endif /* APP_HEADER_H_ */
