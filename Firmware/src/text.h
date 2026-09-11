/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_TEXT_H_
#define APP_TEXT_H_

#include <stdbool.h>
#include <stdint.h>

#include "font_siji.h"

/*
 * Text-grid UI layer on top of the SSD1680 e-paper panel (display.c/.h).
 *
 * Owns a private full-screen 1bpp framebuffer, renders monospace text into
 * it cell-by-cell (fixed rows/cols grid, like a terminal), and tracks which
 * rows changed and by how much. app_text_flush() only ever pushes the
 * changed rows to the panel - each as its own app_display_refresh_partial()
 * call, cropped to the changed column range - so a cursor move or a single
 * row of text changing doesn't repaint the whole screen. Call
 * app_text_full_refresh() periodically instead (e.g. every N page turns)
 * to clear the ghosting that partial refreshes build up over time.
 */

/* Built-in monospace fonts (reusing Zephyr's CFB default font tables - see
 * text.c for why). Index by size, not literal pixel dimensions.
 */
enum app_text_font {
	APP_TEXT_FONT_SMALL = 0,  /* 10x16px cells */
	APP_TEXT_FONT_MEDIUM = 1, /* 15x24px cells */
	APP_TEXT_FONT_LARGE = 2,  /* 20x32px cells */
	APP_TEXT_FONT_TAMZEN = 3, /* 6x16px cells - Tamzen6x12r.bdf, bottom-
				   * padded from its native 12px height to the
				   * next multiple of 8 (see font_tamzen.h).
				   * Chosen size for menu/list UI text - see
				   * the font-selection note in text.c.
				   */
};

/*
 * Allocate the framebuffer (sized to the panel) and select a font. Must be
 * called after app_display_init(). Leaves the framebuffer blank/white with
 * every row marked dirty - call app_text_flush() or app_text_full_refresh()
 * to actually push that to the panel.
 *
 * Returns 0 on success, negative errno otherwise.
 */
int app_text_init(enum app_text_font font);

/*
 * Switch fonts. Reflows the character grid (app_text_get_grid() changes)
 * and clears the framebuffer - it does not re-render whatever was drawn
 * with the previous font.
 */
int app_text_set_font(enum app_text_font font);

/* Character grid size (columns, rows) for the current font. */
void app_text_get_grid(uint16_t *cols, uint16_t *rows);

/* Clear the framebuffer to blank/white and mark every row dirty. Does not
 * touch the panel until app_text_flush()/app_text_full_refresh().
 */
void app_text_clear(void);

/*
 * Draw one row of text, left-aligned, truncated or space-padded to the
 * grid width. Marks the row dirty. Returns -EINVAL if row is out of range.
 */
int app_text_draw_row(uint16_t row, const char *str);

/*
 * Draw a single character cell. Marks that row dirty (cropped to the
 * cell's column range). Returns -EINVAL if row/col is out of range.
 */
int app_text_draw_cell(uint16_t row, uint16_t col, char c);

/*
 * Draw a siji icon glyph (font_siji.h) at a raw pixel x-offset within a
 * grid row - not column-aligned to the current text font's cell width,
 * since icons (12px wide) don't share a cell size with any text font
 * here. Icons are always 12x16px regardless of the active text font;
 * vertical placement is always the top 16px of the row, which only
 * lines up exactly with row content for APP_TEXT_FONT_SMALL/TAMZEN (both
 * 16px tall) - under MEDIUM/LARGE the icon just occupies the top of a
 * taller cell. Marks the row dirty (cropped to the icon's column range).
 * Returns -EINVAL if row is out of range or icon is not a valid
 * app_text_icon value.
 */
int app_text_draw_icon(uint16_t row, uint16_t x, enum app_text_icon icon);

/*
 * Move (or hide) the blinking text cursor, rendered as an inverted
 * character cell. Un-inverts the previous cursor cell first (if it was
 * visible), then inverts the new one if `visible` is true. Marks both
 * cells' rows dirty. Pass visible=false to just hide it.
 *
 * Note: this assumes nothing else redraws the cell currently holding the
 * cursor without going through this function first - doing so would leave
 * a stale inverted glyph behind.
 */
int app_text_set_cursor(uint16_t row, uint16_t col, bool visible);

/*
 * Push every dirty row to the panel - one app_display_refresh_partial()
 * call per dirty row, each cropped to that row's changed column range
 * (snapped to 8px boundaries, which the SSD1680's RAM addressing needs
 * regardless of what app_display_refresh_partial()'s own contract
 * promises - see the comment above flush's crop math in text.c). Clears
 * dirty state on success. No-op if nothing is dirty.
 */
int app_text_flush(void);

/*
 * Force a full-panel refresh (SSD1680 "Mode 1" waveform) of the whole
 * framebuffer, to clear ghosting built up from repeated partial/fast
 * refreshes. Clears all dirty state. This is slow and visibly flashes -
 * call it periodically, not on every update.
 */
int app_text_full_refresh(void);

#endif /* APP_TEXT_H_ */
