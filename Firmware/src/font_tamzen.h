/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FONT_TAMZEN_H_
#define APP_FONT_TAMZEN_H_

#include <zephyr/display/cfb.h>

/*
 * Tamzen6x12r.bdf (sunaku/tamzen-font, Tamsyn-derived terms), converted
 * straight from its BDF glyph bitmaps - no library, no re-rasterizing.
 * Bottom-padded from its native 12px height to 16px (4 blank rows appended
 * below the glyph, i.e. extra line gap, not a shifted baseline) because
 * CFB_FONT_MONO_VPACKED packs each column into whole 8-row byte tiles, and
 * text.c requires every font height to be a multiple of 8 for its
 * framebuffer band math - see the font-height note in text.c. Covers ASCII
 * 32-126, matching Zephyr's built-in CFB fonts.
 */
extern const struct cfb_font font_tamzen_6x16;

#endif /* APP_FONT_TAMZEN_H_ */
