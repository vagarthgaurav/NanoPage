/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/display/cfb.h>

#include "display.h"
#include "font_tamzen.h"
#include "text.h"

/*
 * This reuses Zephyr's built-in CFB font tables (DroidSansMono-derived,
 * registered via FONT_ENTRY_DEFINE in subsys/fb/cfb_fonts.c, pulled in by
 * CONFIG_CHARACTER_FRAMEBUFFER_USE_DEFAULT_FONTS) directly, but does NOT go
 * through the CFB subsystem (subsys/fb/cfb.c) itself:
 *
 *  - cfb_framebuffer_finalize() only ever writes the *entire* framebuffer
 *    to the display - there's no partial-region equivalent - and its
 *    framebuffer state (struct char_framebuffer) is a file-static in cfb.c
 *    with no accessor, so it can't be wired up to
 *    app_display_refresh_partial() from here.
 *  - CFB's own glyph blit (draw_char_vtmono) has to handle glyphs starting
 *    at non-tile-aligned y positions, which is genuinely fiddly. We don't
 *    need that: every row in our fixed text grid starts at row*font.height,
 *    and every built-in font height (16/24/32) is a multiple of 8, so every
 *    glyph we draw is already page-aligned - draw_glyph() below can just
 *    walk pixel by pixel instead of reimplementing CFB's tile-boundary
 *    shifting.
 *
 * TYPE_SECTION_START(cfb_font) is the same iterable-sections mechanism CFB
 * itself uses to enumerate registered fonts (see cfb_get_font_size() in
 * cfb.c) - it's how any translation unit that calls FONT_ENTRY_DEFINE
 * contributes a font, so reading it directly here is using the mechanism
 * as intended, not reaching past it. Font index order matches declaration
 * order in cfb_fonts.c: font1016, font1524, font2032 - same convention
 * Zephyr's own cfb shell (cfb_shell.c) uses to select fonts by index.
 */
STRUCT_SECTION_START_EXTERN(cfb_font);

/*
 * Font selection for menu/list UI text (as opposed to the built-in
 * DroidSansMono-derived fonts above, still used for APP_TEXT_FONT_SMALL/
 * MEDIUM/LARGE): evaluated Terminus and Tamzen, two open-source fixed-
 * pixel bitmap font families, side by side at matching cell sizes.
 * Grayscale (the SSD1680 does support a real 4-gray mode via a custom
 * waveform LUT, confirmed against GxEPD2_4G's own source/notes - contrary
 * to an earlier, incomplete read of the datasheets alone) was ruled out
 * for this: GxEPD2_4G's release notes state this exact panel
 * ("GDEM029T94", the same physical panel as GDEY029T94 under a different
 * reseller's part number) has no partial-refresh support in grey mode, so
 * any grayscale text would cost a full ~1.8s flashing refresh on every
 * update instead of text.c's cheap per-row partial refresh - not
 * acceptable for menu/list navigation, which redraws often. Chose Tamzen
 * at 6x16 (its native 6x12 glyph, bottom-padded to the next multiple of
 * 8 - see font_tamzen.h) over Terminus for its softer, more book-like
 * letterforms, confirmed legible on the real panel at this size.
 *
 * Noto Sans was also evaluated as a book-text candidate (same partial-
 * refresh reasoning above rules out grayscale for book pages too, once
 * page turns need to stay on the fast/partial waveform rather than a full
 * refresh every turn) but shelved for now: it's a proportional outline
 * font with no runtime rasterizer in this firmware, so using it meant
 * force-fitting it into a fixed-width bitmap cell like Tamzen. At a small
 * size (5x8) that came out close to illegible - its outline isn't hinted
 * for that pixel density the way Tamzen's native small sizes are - and at
 * a size that was actually legible (16x24) it was judged too large for
 * body text. Sticking with Tamzen for both menu/list and book text for
 * now; revisit the book font separately later if Tamzen doesn't hold up
 * for long-form reading.
 */

/* Number of Zephyr built-in CFB fonts app_text_init() requires to be linked
 * in (font1016/font1524/font2032 - checked via STRUCT_SECTION_COUNT, not
 * the total font count below, since font_tamzen_6x16 isn't part of that
 * iterable section - see font_tamzen.c for why).
 */
#define APP_TEXT_NUM_BUILTIN_FONTS 3
/* Total selectable fonts, built-in + font_tamzen_6x16 - the valid range
 * for enum app_text_font.
 */
#define APP_TEXT_NUM_FONTS 4
/* 128px panel height / 16px smallest font row = 8 rows, max across fonts. */
#define APP_TEXT_MAX_ROWS 8

struct row_dirty {
	bool dirty;
	uint16_t x_min;
	uint16_t x_max; /* exclusive */
};

/*
 * Framebuffer layout: the SSD1680 driver reports SCREEN_INFO_MONO_VTILED
 * for this board's orientation (drivers/display/ssd16xx.c,
 * ssd16xx_get_capabilities() - confirmed by reading the driver, not just
 * inferred from display.h's now-corrected comments). That means the
 * panel's native RAM format is NOT plain row-major scanlines: the buffer
 * is organized in 8-row-tall bands (fb_height/8 of them), and *within* a
 * band each byte is one pixel *column* (8 vertically-stacked pixels), not
 * 8 horizontally-adjacent ones. So fb_pitch below is bytes-per-band
 * (== fb_width, one byte per column) rather than bytes-per-scanline - a
 * row-major DIV_ROUND_UP(width, 8) pitch (the first thing to reach for)
 * produces scrambled output on this display. A uniform black/white fill
 * can't tell the two layouts apart - every byte is identical either way -
 * which is why this wasn't caught by the earlier full/fast/partial
 * refresh bring-up; it only shows up once real per-pixel content (text)
 * is written.
 */
static uint8_t *fb_buf;
static size_t fb_size;
static uint16_t fb_width;
static uint16_t fb_height;
static uint16_t fb_pitch; /* bytes per 8-row band (== fb_width) */

static enum app_text_font cur_font;
static uint16_t grid_cols;
static uint16_t grid_rows;

/*
 * ssd16xx_set_window() (drivers/display/ssd16xx.c) re-derives the physical
 * *y* position for ROTATED_180 (bakes the orientation into the row/page
 * address it sends the controller), but for *x* it only reverses the
 * controller's address-counter scan direction within the same
 * [x, x+width) window - it never relocates the window itself. Invisible
 * for a full-width write (x=0 mirrors to itself), but any narrower
 * app_display_refresh_partial() rectangle needs x mirrored by hand or it
 * lands at the wrong physical column - see the longer note on
 * app_display_refresh_partial() in display.h. This is what caused the
 * text cursor to visibly move in the wrong direction (and appear to not
 * un-invert) before this was added.
 */
static bool mirror_x;

static struct row_dirty rows_dirty[APP_TEXT_MAX_ROWS];

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

	return &TYPE_SECTION_START(cfb_font)[font];
}

static void mark_dirty(uint16_t row, uint16_t x_min, uint16_t x_max)
{
	struct row_dirty *d;

	if (row >= grid_rows) {
		return;
	}

	d = &rows_dirty[row];
	if (!d->dirty) {
		d->dirty = true;
		d->x_min = x_min;
		d->x_max = x_max;
	} else {
		d->x_min = MIN(d->x_min, x_min);
		d->x_max = MAX(d->x_max, x_max);
	}
}

static inline uint8_t *pixel_byte(uint16_t x, uint16_t y)
{
	return &fb_buf[(size_t)(y / 8) * fb_pitch + x];
}

static inline void set_pixel(uint16_t x, uint16_t y, bool black)
{
	uint8_t *byte = pixel_byte(x, y);
	/* MSB-first within a band byte (SCREEN_INFO_MONO_MSB_FIRST) - bit
	 * (7 - y%8) is the top-of-tile pixel, matching cfb.c's draw_point().
	 */
	uint8_t mask = BIT(7 - (y % 8));

	/* bit=1 -> white, bit=0 -> black - the panel-native RAM convention
	 * (see display.c); orthogonal to the vtiled-vs-row-major layout
	 * question above.
	 */
	if (black) {
		*byte &= ~mask;
	} else {
		*byte |= mask;
	}
}

/*
 * Blit one glyph, background included (every pixel in the cell is written,
 * not OR'd), so redrawing a cell always fully replaces whatever was there
 * before - no separate clear step needed.
 *
 * Font glyph data is CFB_FONT_MONO_VPACKED: byte (gx, gy/8) holds 8
 * vertically-stacked pixels of column gx, bit (gy % 8) = 1 means that
 * pixel is glyph ink (bit0 = topmost row of the 8-row tile). Verified
 * against cfb.c's draw_char_vtmono()/draw_point() by tracing through their
 * MSB-first byte_reverse() handling for a tile-aligned glyph (our case):
 * the two reversals cancel out, leaving glyph bit N = row N regardless of
 * screen_info - this is the font's own, buffer-layout-independent bit
 * order, separate from the vtiled/pixel_byte() question above.
 */
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

/*
 * Same bit-reading convention as draw_glyph() above, but only blits the
 * glyph's first `native_height` rows (the real ink, per the font's own
 * documented padding - e.g. FONT_TAMZEN_NATIVE_HEIGHT), placed at the
 * *bottom* of the `cell_height`-tall cell instead of the top - and
 * explicitly clears the now-blank rows above it, since a redraw must
 * still fully replace whatever was in the cell before (same contract as
 * draw_glyph()).
 */
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

	mark_dirty(row, px, px + f->width);
}

void app_text_clear(void)
{
	if (!fb_buf) {
		return;
	}

	memset(fb_buf, 0xFF, fb_size);

	for (uint16_t row = 0; row < APP_TEXT_MAX_ROWS; row++) {
		rows_dirty[row].dirty = (row < grid_rows);
		rows_dirty[row].x_min = 0;
		rows_dirty[row].x_max = fb_width;
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
		/* Every band must be a full 8 rows - pixel_byte()/flush() assume
		 * this, and the SSD1680 driver itself requires panel heights
		 * that are a multiple of 8 (EPD_PANEL_NUMOF_ROWS_PER_PAGE).
		 */
		printk("text: display height %u not a multiple of 8\n", height);
		return -EINVAL;
	}

	fb_width = width;
	fb_height = height;
	fb_pitch = width; /* one byte per column per 8-row band */
	fb_size = (size_t)fb_pitch * (height / 8U);

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
	mark_dirty(row, px, px + f->width);

	/* Redrawing over the cursor cell leaves a stale inverted glyph -
	 * drop cursor tracking rather than let that happen silently.
	 */
	if (cursor.visible && cursor.row == row && cursor.col == col) {
		cursor.visible = false;
	}

	return 0;
}

/*
 * draw_glyph() falls back to ' ' for a char outside f->first_char..
 * f->last_char - safe for a text font (space is always in range), but
 * font_siji_icons only spans 0..APP_TEXT_ICON_BATTERY_MAX (12), so ' '
 * (32) would itself be out of range and read past the glyph array. Bounds-
 * check the icon here instead of relying on that fallback.
 */
int app_text_draw_icon(uint16_t row, uint16_t x, enum app_text_icon icon)
{
	uint16_t py;

	if (!fb_buf || row >= grid_rows) {
		return -EINVAL;
	}

	if (icon > APP_TEXT_ICON_BATTERY_MAX) {
		return -EINVAL;
	}

	py = row * font_entry(cur_font)->height;

	draw_glyph(&font_siji_icons, (unsigned char)icon, x, py);
	mark_dirty(row, x, x + font_siji_icons.width);

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
	uint16_t py;

	if (!fb_buf || row >= grid_rows || y_in_row >= font_entry(cur_font)->height) {
		return -EINVAL;
	}

	py = row * font_entry(cur_font)->height + y_in_row;

	for (uint16_t x = 0; x < fb_width; x++) {
		set_pixel(x, py, black);
	}

	mark_dirty(row, 0, fb_width);

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

	mark_dirty(row, 0, fb_width);

	/* Same reasoning as app_text_draw_cell(): redrawing over the cursor
	 * cell leaves a stale inverted glyph.
	 */
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
	const struct cfb_font *f = font_entry(cur_font);
	uint16_t row = 0;

	while (row < grid_rows) {
		uint16_t run_start, run_end, x_min, x_max;
		uint16_t x, w, y, h, band0, nbands;
		size_t buf_size;
		uint8_t *tmp;
		int ret;

		if (!rows_dirty[row].dirty) {
			row++;
			continue;
		}

		/*
		 * Coalesce a run of consecutive dirty rows into ONE combined
		 * app_display_refresh_partial() call instead of one per row.
		 * Each SSD1680 partial refresh pays its own fixed BUSY-wait
		 * (~530ms measured, largely independent of rectangle size -
		 * it's the waveform settling, not the SPI clock-out, that
		 * dominates) regardless of how small the changed region is,
		 * so flushing N adjacent rows separately costs ~N times what
		 * one combined taller rectangle does. This matters most for
		 * two callers: a cursor move (browser.c) dirties exactly two
		 * *adjacent* rows (old + new cursor position, always one row
		 * apart within a page), and a full-page redraw (also
		 * browser.c) dirties every visible content row at once - both
		 * used to show up as a visibly slow, one-row-at-a-time
		 * flicker; a run merges into a single refresh instead. The
		 * combined rectangle's x-range is the union of every row's
		 * own dirty x-range in the run - rows with a narrower range
		 * than the run's widest just resend a bit of already-correct
		 * pixel data alongside, harmless (no visual difference, only
		 * a slightly bigger SPI transfer).
		 */
		run_start = row;
		x_min = rows_dirty[row].x_min;
		x_max = rows_dirty[row].x_max;
		row++;
		while (row < grid_rows && rows_dirty[row].dirty) {
			x_min = MIN(x_min, rows_dirty[row].x_min);
			x_max = MAX(x_max, rows_dirty[row].x_max);
			row++;
		}
		run_end = row;

		/*
		 * x/width need no alignment here - in this vtiled layout each
		 * byte is already exactly one pixel column, so any x/width is
		 * byte-exact by construction. That also matches
		 * ssd16xx_set_window()'s bounds check (no x/width % 8
		 * requirement in this orientation) and its write path's
		 * `height * width / 8` byte-count math, which is exact as
		 * long as height is a multiple of 8 - guaranteed since h is a
		 * whole number of row heights (16/24/32 for the built-in
		 * fonts), each already a multiple of 8.
		 */
		x = x_min;
		w = x_max - x_min;
		y = run_start * f->height;
		h = (uint16_t)(run_end - run_start) * f->height;

		band0 = y / 8;
		nbands = h / 8;
		buf_size = (size_t)w * nbands;

		tmp = k_malloc(buf_size);
		if (!tmp) {
			return -ENOMEM;
		}

		for (uint16_t b = 0; b < nbands; b++) {
			memcpy(&tmp[(size_t)b * w], &fb_buf[(size_t)(band0 + b) * fb_pitch + x],
			       w);
		}

		/* See the mirror_x comment above - the panel's x position, not
		 * our column content, needs mirroring here.
		 */
		ret = app_display_refresh_partial(mirror_x ? fb_width - x - w : x, y, w, h, tmp,
						   buf_size);
		k_free(tmp);

		if (ret) {
			return ret;
		}

		for (uint16_t r = run_start; r < run_end; r++) {
			rows_dirty[r].dirty = false;
		}
	}

	return 0;
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

	for (uint16_t row = 0; row < grid_rows; row++) {
		rows_dirty[row].dirty = false;
	}

	return 0;
}
