/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_TEXT_H_
#define APP_TEXT_H_

#include <stdbool.h>
#include <stdint.h>

#include "font_siji.h"

enum app_text_font {
	APP_TEXT_FONT_SMALL = 0,  /* 10x16px cells */
	APP_TEXT_FONT_MEDIUM = 1, /* 15x24px cells */
	APP_TEXT_FONT_LARGE = 2,  /* 20x32px cells */
	APP_TEXT_FONT_TAMZEN = 3,
	APP_TEXT_FONT_TAMZEN_MEDIUM = 4,
	APP_TEXT_FONT_TAMZEN_LARGE = 5,
};

int app_text_init(enum app_text_font font);

int app_text_set_font(enum app_text_font font);

void app_text_get_grid(uint16_t *cols, uint16_t *rows);

void app_text_get_content_rows(uint16_t *start_row, uint16_t *rows);

void app_text_font_info(enum app_text_font font, uint16_t *glyph_width, uint16_t *glyph_height,
			 uint16_t *cols);

void app_text_clear(void);

int app_text_draw_row(uint16_t row, const char *str);

int app_text_draw_cell(uint16_t row, uint16_t col, char c);

uint16_t app_text_col_to_px(uint16_t col);

uint16_t app_text_row_height(void);

int app_text_draw_row_bottom_aligned(uint16_t row, const char *str, uint16_t native_height);

int app_text_draw_text_px(uint16_t y_px, const char *str, enum app_text_font font);

int app_text_draw_icon(uint16_t row, uint16_t x, enum app_text_icon icon);

int app_text_set_cursor(uint16_t row, uint16_t col, bool visible);

int app_text_draw_hline(uint16_t row, uint16_t y_in_row, bool black);

int app_text_draw_hline_span(uint16_t row, uint16_t y_in_row, uint16_t x0, uint16_t x1_exclusive,
			     bool black);

uint16_t app_text_width_px(void);

int app_text_flush(void);

int app_text_full_refresh(void);

#endif /* APP_TEXT_H_ */
