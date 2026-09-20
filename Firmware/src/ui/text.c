/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/display/cfb.h>

#include "display.h"
#include "font_tamzen.h"
#include "text.h"

STRUCT_SECTION_START_EXTERN(cfb_font);

#define APP_TEXT_NUM_BUILTIN_FONTS 3
#define APP_TEXT_NUM_FONTS 6
#define APP_TEXT_MAX_ROWS 8
#define APP_TEXT_MAX_BANDS 16

struct band_dirty {
	bool dirty;
	uint16_t x_min;
	uint16_t x_max; /* exclusive */
};

static uint8_t *fb_buf;
static size_t fb_size;
static uint16_t fb_width;
static uint16_t fb_height;
static uint16_t fb_pitch; /* bytes per 8-row band (== fb_width) */

static enum app_text_font cur_font;
static uint16_t grid_cols;
static uint16_t grid_rows;

static bool mirror_x;

static uint16_t total_bands; /* fb_height / 8, set once in app_text_init() */
static struct band_dirty bands_dirty[APP_TEXT_MAX_BANDS];

static struct {
	bool visible;
	uint16_t row;
	uint16_t col;
} cursor;

static inline const struct cfb_font *font_entry(enum app_text_font font)
{
	if (font == APP_TEXT_FONT_TAMZEN) {
		return &font_tamzen_6x16;
	}
	if (font == APP_TEXT_FONT_TAMZEN_MEDIUM) {
		return &font_tamzen_8x16;
	}
	if (font == APP_TEXT_FONT_TAMZEN_LARGE) {
		return &font_tamzen_10x24;
	}

	return &TYPE_SECTION_START(cfb_font)[font];
}

static void mark_dirty_px(uint16_t y, uint16_t height, uint16_t x_min, uint16_t x_max)
{
	uint16_t band0 = y / 8;
	uint16_t nbands = height / 8;

	for (uint16_t b = band0; b < band0 + nbands && b < total_bands; b++) {
		struct band_dirty *d = &bands_dirty[b];

		if (!d->dirty) {
			d->dirty = true;
			d->x_min = x_min;
			d->x_max = x_max;
		} else {
			d->x_min = MIN(d->x_min, x_min);
			d->x_max = MAX(d->x_max, x_max);
		}
	}
}

static inline uint8_t *pixel_byte(uint16_t x, uint16_t y)
{
	return &fb_buf[(size_t)(y / 8) * fb_pitch + x];
}

static inline void set_pixel(uint16_t x, uint16_t y, bool black)
{
	uint8_t *byte = pixel_byte(x, y);
	uint8_t mask = BIT(7 - (y % 8));

	if (black) {
		*byte &= ~mask;
	} else {
		*byte |= mask;
	}
}

static void draw_glyph(const struct cfb_font *f, unsigned char c, uint16_t px, uint16_t py)
{
	const uint8_t *glyph;

	if (c < f->first_char || c > f->last_char) {
		c = ' ';
	}

	glyph = (const uint8_t *)f->data + (c - f->first_char) * (f->width * f->height / 8U);

	for (uint16_t gx = 0; gx < f->width; gx++) {
		for (uint16_t gy = 0; gy < f->height; gy++) {
			uint8_t byte = glyph[gx * (f->height / 8U) + gy / 8U];
			bool ink = (byte >> (gy % 8U)) & 1U;

			set_pixel(px + gx, py + gy, ink);
		}
	}
}

static void draw_glyph_bottom(const struct cfb_font *f, unsigned char c, uint16_t px,
			       uint16_t py_top, uint16_t cell_height, uint16_t native_height)
{
	const uint8_t *glyph;
	uint16_t pad = cell_height - native_height;

	if (c < f->first_char || c > f->last_char) {
		c = ' ';
	}

	glyph = (const uint8_t *)f->data + (c - f->first_char) * (f->width * f->height / 8U);

	for (uint16_t gx = 0; gx < f->width; gx++) {
		for (uint16_t y = 0; y < pad; y++) {
			set_pixel(px + gx, py_top + y, false);
		}

		for (uint16_t gy = 0; gy < native_height; gy++) {
			uint8_t byte = glyph[gx * (f->height / 8U) + gy / 8U];
			bool ink = (byte >> (gy % 8U)) & 1U;

			set_pixel(px + gx, py_top + pad + gy, ink);
		}
	}
}

static void invert_cell(uint16_t row, uint16_t col)
{
	const struct cfb_font *f = font_entry(cur_font);
	uint16_t px = col * f->width;
	uint16_t py = row * f->height;

	for (uint16_t gx = 0; gx < f->width; gx++) {
		for (uint16_t gy = 0; gy < f->height; gy++) {
			*pixel_byte(px + gx, py + gy) ^= BIT(7 - ((py + gy) % 8));
		}
	}

	mark_dirty_px(py, f->height, px, px + f->width);
}

void app_text_clear(void)
{
	if (!fb_buf) {
		return;
	}

	memset(fb_buf, 0xFF, fb_size);

	for (uint16_t b = 0; b < total_bands; b++) {
		bands_dirty[b].dirty = true;
		bands_dirty[b].x_min = 0;
		bands_dirty[b].x_max = fb_width;
	}

	cursor.visible = false;
}

int app_text_set_font(enum app_text_font font)
{
	const struct cfb_font *f;

	if (font >= APP_TEXT_NUM_FONTS) {
		return -EINVAL;
	}

	f = font_entry(font);
	cur_font = font;
	grid_cols = fb_width / f->width;
	grid_rows = MIN(fb_height / f->height, APP_TEXT_MAX_ROWS);

	app_text_clear();

	return 0;
}

int app_text_init(enum app_text_font font)
{
	uint16_t width, height;
	uint8_t numof_fonts;
	int ret;

	STRUCT_SECTION_COUNT(cfb_font, &numof_fonts);
	if (numof_fonts < APP_TEXT_NUM_BUILTIN_FONTS) {
		printk("text: expected %d built-in CFB fonts, found %u - "
		       "is CONFIG_CHARACTER_FRAMEBUFFER_USE_DEFAULT_FONTS set?\n",
		       APP_TEXT_NUM_BUILTIN_FONTS, numof_fonts);
		return -ENODEV;
	}

	app_display_get_size(&width, &height);
	mirror_x = (app_display_get_orientation() == DISPLAY_ORIENTATION_ROTATED_180);

	if (height % 8 != 0) {
		printk("text: display height %u not a multiple of 8\n", height);
		return -EINVAL;
	}

	fb_width = width;
	fb_height = height;
	fb_pitch = width; /* one byte per column per 8-row band */
	fb_size = (size_t)fb_pitch * (height / 8U);
	total_bands = MIN(height / 8U, APP_TEXT_MAX_BANDS);

	fb_buf = k_malloc(fb_size);
	if (!fb_buf) {
		return -ENOMEM;
	}

	ret = app_text_set_font(font);
	if (ret) {
		k_free(fb_buf);
		fb_buf = NULL;
		return ret;
	}

	return 0;
}

void app_text_get_grid(uint16_t *cols, uint16_t *rows)
{
	*cols = grid_cols;
	*rows = grid_rows;
}

void app_text_font_info(enum app_text_font font, uint16_t *glyph_width, uint16_t *glyph_height,
			 uint16_t *cols)
{
	const struct cfb_font *f = font_entry(font);

	*glyph_width = f->width;
	*glyph_height = f->height;
	*cols = fb_width / f->width;
}

int app_text_draw_cell(uint16_t row, uint16_t col, char c)
{
	const struct cfb_font *f;
	uint16_t px, py;

	if (!fb_buf || row >= grid_rows || col >= grid_cols) {
		return -EINVAL;
	}

	f = font_entry(cur_font);
	px = col * f->width;
	py = row * f->height;

	draw_glyph(f, (unsigned char)c, px, py);
	mark_dirty_px(py, f->height, px, px + f->width);

	if (cursor.visible && cursor.row == row && cursor.col == col) {
		cursor.visible = false;
	}

	return 0;
}

int app_text_draw_icon(uint16_t row, uint16_t x, enum app_text_icon icon)
{
	uint16_t py, row_height;

	if (!fb_buf || row >= grid_rows) {
		return -EINVAL;
	}

	if (icon > font_siji_icons.last_char) {
		return -EINVAL;
	}

	row_height = font_entry(cur_font)->height;
	py = row * row_height;

	draw_glyph(&font_siji_icons, (unsigned char)icon, x, py);
	mark_dirty_px(py, row_height, x, x + font_siji_icons.width);

	return 0;
}

uint16_t app_text_col_to_px(uint16_t col)
{
	return col * font_entry(cur_font)->width;
}

uint16_t app_text_row_height(void)
{
	return font_entry(cur_font)->height;
}

int app_text_draw_hline(uint16_t row, uint16_t y_in_row, bool black)
{
	uint16_t row_height = font_entry(cur_font)->height;
	uint16_t py;

	if (!fb_buf || row >= grid_rows || y_in_row >= row_height) {
		return -EINVAL;
	}

	py = row * row_height + y_in_row;

	for (uint16_t x = 0; x < fb_width; x++) {
		set_pixel(x, py, black);
	}

	mark_dirty_px(row * row_height, row_height, 0, fb_width);

	return 0;
}

int app_text_draw_row(uint16_t row, const char *str)
{
	size_t len;

	if (!fb_buf || row >= grid_rows) {
		return -EINVAL;
	}

	len = strlen(str);

	for (uint16_t col = 0; col < grid_cols; col++) {
		app_text_draw_cell(row, col, (col < len) ? str[col] : ' ');
	}

	return 0;
}

int app_text_draw_text_px(uint16_t y_px, const char *str, enum app_text_font font)
{
	const struct cfb_font *f = font_entry(font);
	uint16_t cols;
	size_t len;

	if (!fb_buf || y_px % 8 != 0 || (uint32_t)y_px + f->height > fb_height) {
		return -EINVAL;
	}

	cols = fb_width / f->width;
	len = strlen(str);

	for (uint16_t col = 0; col < cols; col++) {
		unsigned char c = (col < len) ? (unsigned char)str[col] : ' ';

		draw_glyph(f, c, col * f->width, y_px);
	}

	mark_dirty_px(y_px, f->height, 0, fb_width);

	return 0;
}

int app_text_draw_row_bottom_aligned(uint16_t row, const char *str, uint16_t native_height)
{
	const struct cfb_font *f;
	size_t len;
	uint16_t py;

	if (!fb_buf || row >= grid_rows) {
		return -EINVAL;
	}

	f = font_entry(cur_font);
	if (native_height > f->height) {
		return -EINVAL;
	}

	len = strlen(str);
	py = row * f->height;

	for (uint16_t col = 0; col < grid_cols; col++) {
		unsigned char c = (col < len) ? (unsigned char)str[col] : ' ';

		draw_glyph_bottom(f, c, col * f->width, py, f->height, native_height);
	}

	mark_dirty_px(py, f->height, 0, fb_width);

	if (cursor.visible && cursor.row == row) {
		cursor.visible = false;
	}

	return 0;
}

int app_text_set_cursor(uint16_t row, uint16_t col, bool visible)
{
	if (!fb_buf) {
		return -EINVAL;
	}

	if (visible && (row >= grid_rows || col >= grid_cols)) {
		return -EINVAL;
	}

	if (cursor.visible) {
		invert_cell(cursor.row, cursor.col);
	}

	cursor.visible = visible;
	cursor.row = row;
	cursor.col = col;

	if (visible) {
		invert_cell(row, col);
	}

	return 0;
}

int app_text_flush(void)
{
	uint16_t band = 0;
	int ret = 0;

	/*
	 * Bracket the whole multi-run flush in one lock/unlock pair so disjoint
	 * dirty regions share a single SD power-up instead of each triggering
	 * its own app_display_lock()-driven SD power cycle (and its 20ms
	 * settle delay) via app_display_refresh_partial().
	 */
	app_display_lock();

	while (band < total_bands) {
		uint16_t run_start, run_end, x_min, x_max;
		uint16_t x, w, y, h, nbands;
		size_t buf_size;
		uint8_t *tmp;

		if (!bands_dirty[band].dirty) {
			band++;
			continue;
		}

		run_start = band;
		x_min = bands_dirty[band].x_min;
		x_max = bands_dirty[band].x_max;
		band++;
		while (band < total_bands && bands_dirty[band].dirty) {
			x_min = MIN(x_min, bands_dirty[band].x_min);
			x_max = MAX(x_max, bands_dirty[band].x_max);
			band++;
		}
		run_end = band;

		x = x_min;
		w = x_max - x_min;
		y = run_start * 8;
		nbands = run_end - run_start;
		h = nbands * 8;

		buf_size = (size_t)w * nbands;

		tmp = k_malloc(buf_size);
		if (!tmp) {
			ret = -ENOMEM;
			break;
		}

		for (uint16_t b = 0; b < nbands; b++) {
			memcpy(&tmp[(size_t)b * w],
			       &fb_buf[(size_t)(run_start + b) * fb_pitch + x], w);
		}

		ret = app_display_refresh_partial(mirror_x ? fb_width - x - w : x, y, w, h, tmp,
						   buf_size);
		k_free(tmp);

		if (ret) {
			break;
		}

		for (uint16_t b = run_start; b < run_end; b++) {
			bands_dirty[b].dirty = false;
		}
	}

	app_display_unlock();

	return ret;
}

int app_text_full_refresh(void)
{
	int ret;

	if (!fb_buf) {
		return -EINVAL;
	}

	ret = app_display_refresh_full(fb_buf, fb_size);
	if (ret) {
		return ret;
	}

	for (uint16_t b = 0; b < total_bands; b++) {
		bands_dirty[b].dirty = false;
	}

	return 0;
}
