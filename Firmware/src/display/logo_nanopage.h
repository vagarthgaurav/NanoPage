/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_LOGO_NANOPAGE_H_
#define APP_LOGO_NANOPAGE_H_

#include <stdint.h>

/*
 * Full-screen (296x128) NanoPage wordmark, pre-rendered from
 * Pictures/NanoPage.svg into the vtiled buffer format
 * app_display_refresh_full() expects (see display.h and the generation
 * comment in logo_nanopage.c). Pass straight to app_display_refresh_full().
 */
extern const uint8_t app_logo_nanopage[4736];

#endif /* APP_LOGO_NANOPAGE_H_ */
