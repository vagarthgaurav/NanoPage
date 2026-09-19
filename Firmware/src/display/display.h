/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_DISPLAY_H_
#define APP_DISPLAY_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/display.h>

int app_display_init(void);

void app_display_lock(void);
void app_display_unlock(void);

void app_display_get_size(uint16_t *width, uint16_t *height);

enum display_orientation app_display_get_orientation(void);

int app_display_refresh_full(const uint8_t *buf, size_t buf_size);

int app_display_refresh_fast(const uint8_t *buf, size_t buf_size);

int app_display_refresh_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
				 const uint8_t *buf, size_t buf_size);

int app_display_deep_sleep(void);

#endif /* APP_DISPLAY_H_ */
