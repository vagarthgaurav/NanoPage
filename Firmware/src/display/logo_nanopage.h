/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_LOGO_NANOPAGE_H_
#define APP_LOGO_NANOPAGE_H_

#include <stdint.h>

#define APP_LOGO_NANOPAGE_WIDTH  70
#define APP_LOGO_NANOPAGE_HEIGHT 16

/*
 * Bitmap for one 16px text row, framebuffer page layout: 2 pages of 70 bytes,
 * bit 7 = top pixel, bit set = white.
 */
extern const uint8_t app_logo_nanopage[APP_LOGO_NANOPAGE_WIDTH * APP_LOGO_NANOPAGE_HEIGHT / 8];

#endif /* APP_LOGO_NANOPAGE_H_ */
