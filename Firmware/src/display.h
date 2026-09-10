/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/display.h>

/* Bring up the e-paper panel (SSD1680) and check the device is ready.
 * Returns 0 on success, negative errno on failure.
 */
int app_display_init(void);

/* Panel dimensions in pixels, for sizing the buffers passed to the
 * refresh functions below.
 */
void app_display_get_size(uint16_t *width, uint16_t *height);

/* Current orientation, as configured by the panel's devicetree `rotation`
 * property (see nanopage.dts). Needed by callers doing their own partial
 * refreshes - see the x-position note on app_display_refresh_partial()
 * below.
 */
enum display_orientation app_display_get_orientation(void);

/*
 * Buffer format used by all three refresh functions below (confirmed by
 * reading drivers/display/ssd16xx.c's ssd16xx_get_capabilities(), which
 * reports SCREEN_INFO_MONO_VTILED for this board's orientation - not the
 * plain row-major layout earlier revisions of these comments described,
 * which only looked correct because the original bring-up test only ever
 * wrote uniform black/white fills, where every byte is identical either
 * way): the buffer is organized as (height / 8) vertically-tiled bands,
 * each `width` bytes long - i.e. one byte per pixel *column*, holding 8
 * vertically-stacked pixel rows, MSB-first (bit (7 - row%8) is the
 * topmost pixel of that 8-row band). `height` must be a multiple of 8;
 * buffer size is then exactly `width * (height / 8)` bytes, no rounding.
 * Within a byte, bit=1 -> white, bit=0 -> black (see the comment in
 * display.c on why this is inverted from Zephyr's PIXEL_FORMAT_MONO10
 * doc comment).
 */

/*
 * Full refresh: forces the SSD1680's ghost-free "full" waveform (OTP
 * Mode 1) over the whole screen - a multi-second, visibly flashing
 * update. Use this periodically (e.g. every N page turns) to clear
 * ghosting left behind by fast/partial refreshes.
 *
 * `buf` must be a full-screen buffer per app_display_get_size(), in the
 * format described above.
 *
 * Returns 0 on success, negative errno on failure.
 */
int app_display_refresh_full(const uint8_t *buf, size_t buf_size);

/*
 * Fast refresh: the SSD1680's quicker "partial" waveform (OTP Mode 2),
 * applied to the whole screen - a sub-second to ~1s update with much
 * less flashing than app_display_refresh_full(), at the cost of
 * ghosting building up over repeated use. Buffer sizing/format is the
 * same as app_display_refresh_full(). Falls back to a full refresh if
 * the devicetree "partial" profile isn't configured on this panel node.
 *
 * Returns 0 on success, negative errno on failure.
 */
int app_display_refresh_fast(const uint8_t *buf, size_t buf_size);

/*
 * Partial refresh: the same Mode 2 waveform as app_display_refresh_fast(),
 * limited to one rectangle - for updating just the part of the screen
 * that changed (a page number, a clock), without redrawing everything.
 *
 * `y` and `height` must each be a multiple of 8 (the panel is
 * vertically tiled in 8-row bands); `x`/`width` have no such
 * restriction - every x/width is already byte-exact in this layout,
 * since one byte is one column. The rectangle must fit within the
 * panel. `buf` must hold `width * (height / 8)` bytes in the format
 * described above.
 *
 * Rotation quirk (found via text.c's cursor, not documented anywhere
 * upstream): ssd16xx_set_window() in the driver correctly re-derives the
 * physical *y* position for ROTATED_180 (it bakes an orientation-
 * dependent reflection into the row/page address it sends the
 * controller), but for *x* it only reverses the controller's address-
 * counter scan direction within the same [x, x+width) window - it never
 * moves the window itself to the reflected position. For a full-width
 * write (x=0) this is invisible (the window already spans the whole
 * panel). For any narrower rectangle under ROTATED_180, the caller must
 * mirror x itself - pass `panel_width - x - width` (panel_width from
 * app_display_get_size()) instead of `x` - or the rectangle lands at the
 * wrong physical spot.
 * app_text_flush() in text.c does this; nothing else here does it for
 * you.
 *
 * Returns 0 on success, negative errno on failure.
 */
int app_display_refresh_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *buf, size_t buf_size);

/* Fill the whole screen solid black (or white if false) and push it with
 * a full refresh (see app_display_refresh_full()). Convenience wrapper
 * used for the bring-up test in main.c.
 * Returns 0 on success, negative errno on failure.
 */
int app_display_clear(bool black);

#endif /* APP_DISPLAY_H_ */
