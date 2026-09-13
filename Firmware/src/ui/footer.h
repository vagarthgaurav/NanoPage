/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FOOTER_H_
#define APP_FOOTER_H_

/*
 * Footer: the last row of the current text grid - the row bordering the
 * panel's physical bottom edge. Battery/current status bar (percentage,
 * voltage, instantaneous + average current from the fuel gauge),
 * bottom-aligned within its cell so the text sits flush against the
 * screen edge (see app_text_draw_row_bottom_aligned() in text.h) rather
 * than the header's top-aligned convention.
 *
 * Draws into text.c's framebuffer only - caller still owns pushing it to
 * the panel (app_text_flush() for a cheap partial update,
 * app_text_full_refresh() for the initial ghost-free draw), same
 * convention as header.c.
 */

/* Locates the footer row (the text grid's last row) - call once, after
 * app_text_init().
 */
void app_footer_init(void);

/*
 * Samples the fuel gauge's instantaneous Current register ahead of the
 * next app_footer_draw(), while the board is otherwise idle. Unlike
 * AvgCurrent (chip-filtered, immune to this), a raw Current sample taken
 * right at draw/refresh time would land on top of the SSD1680 partial-
 * refresh SPI burst that follows a moment later, reading elevated/noisy
 * current instead of true idle draw - so the caller is expected to call
 * this some seconds before app_footer_draw(), with nothing else going on
 * in between (see STATUS_CURRENT_SAMPLE_LEAD_SEC in main.c). Leaves the
 * last sampled value unchanged on I2C failure.
 */
void app_footer_sample_current(void);

/* Redraws the footer row using the fuel gauge's current live state plus
 * whatever app_footer_sample_current() last captured. Returns 0 on
 * success, negative errno if the fuel gauge read failed (row is left
 * untouched in that case).
 */
int app_footer_draw(void);

#endif /* APP_FOOTER_H_ */
