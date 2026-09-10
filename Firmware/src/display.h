/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bring up the e-paper panel (SSD1680) and check the device is ready.
 * Returns 0 on success, negative errno on failure.
 */
int app_display_init(void);

/* Panel dimensions in pixels, for sizing the buffers passed to the
 * refresh functions below.
 */
void app_display_get_size(uint16_t *width, uint16_t *height);

/*
 * Full refresh: forces the SSD1680's ghost-free "full" waveform (OTP
 * Mode 1) over the whole screen - a multi-second, visibly flashing
 * update. Use this periodically (e.g. every N page turns) to clear
 * ghosting left behind by fast/partial refreshes.
 *
 * `buf` must be a full-screen buffer in the panel's native format: 1
 * bit/pixel, MSB first, row-major (bit=1 -> white, bit=0 -> black - see
 * the comment in display.c on why this is inverted from Zephyr's
 * PIXEL_FORMAT_MONO10 doc comment), at least
 * DIV_ROUND_UP(width, 8) * height bytes per app_display_get_size().
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
 * vertically tiled in 8-row pages); `x`/`width` have no such
 * restriction. The rectangle must fit within the panel. `buf` must hold
 * DIV_ROUND_UP(width, 8) * height bytes in the same format as
 * app_display_refresh_full().
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
