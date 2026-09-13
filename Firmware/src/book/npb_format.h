/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_FORMAT_H_
#define APP_BOOK_NPB_FORMAT_H_

#include <stdint.h>

/*
 * NanoPage Book (.npb) - on-disk format only, no reader logic here.
 *
 * EPUB (zipped XHTML/CSS) was evaluated and rejected as an on-device
 * format: parsing it costs more RAM/CPU than this board's battery-life
 * goal can afford (see .claude/CLAUDE.md's "Book format decision"). The
 * fix is to move every expensive step - HTML parsing, word-wrapping,
 * pagination - into a one-time PC-side conversion from a source EPUB into
 * this format, so the firmware never does more than index a table and
 * blit already-decided characters through text.c's grid renderer
 * (app_text_draw_row()). See docs/book-format.md for the full spec,
 * rationale, and the EPUB conversion pipeline this format assumes.
 *
 * Design in one sentence: word-wrapping happens once, offline, per font
 * size; the device just walks a table of (offset, length) spans into a
 * shared plain-text blob. Two things fall out of that:
 *
 *   - PAGE TURN = pure array-index math. Advance the current row index
 *     by however many content rows the active screen layout has, look up
 *     that many struct npb_row entries, seek+read those bytes, draw each
 *     with app_text_draw_row(). No line-wrap/reflow ever runs on-device.
 *
 *   - COLUMNS are fixed per font variant at conversion time (296px panel
 *     width / glyph width never changes), but ROWS-PER-PAGE is
 *     deliberately NOT stored anywhere in this format - it's a
 *     device-side runtime choice (how many rows the reader screen's
 *     layout leaves free for body text), so a chrome change (e.g.
 *     whether a header/footer stays visible while reading) never forces
 *     re-converting every book.
 *
 * The text blob has no embedded delimiters at all - every row's exact
 * content is fully described by its own (offset, length) pair. That's
 * what lets every font variant's row table reference the *same* shared
 * blob even though each wraps the words differently: a "row" is just a
 * different window over one underlying character stream, not a separate
 * copy of it. A blank/paragraph-spacer row is pure row-table metadata
 * (length 0, consumes no blob bytes).
 *
 * Because blob offsets are shared across variants and increase
 * monotonically within a variant's own row table, a single primitive -
 * binary-search a variant's row table for the row spanning a given blob
 * offset - covers resuming last position, jumping to a chapter (struct
 * npb_chapter below), and jumping to a user bookmark (see
 * npb_bookmark_format.h) identically, and survives switching font size
 * mid-book.
 *
 * All multi-byte integers are little-endian, matching the nRF52840
 * natively - no byteswapping needed on load. Every struct below is
 * intentionally sized/padded so a packed on-device parser needs no
 * compiler-inserted padding; reserved fields must be written as 0 by the
 * converter and ignored (not rejected) by the reader, to allow future
 * versions to grow into them.
 */

#define NPB_MAGIC        "NPBK" /* 4 bytes on disk, no NUL terminator */
#define NPB_VERSION      1
#define NPB_MAX_VARIANTS 8 /* room to grow past the initial 3 sizes */

/*
 * Font sizes this format can pre-paginate for. Deliberately a separate
 * enum from text.c's enum app_text_font (SMALL/MEDIUM/LARGE/TAMZEN) -
 * the firmware's font table can change without this format changing.
 * Cutting the actual font_tamzen_8x16/font_tamzen_10x24 glyph bitmaps is
 * separate font/firmware work (same BDF-to-CFB pipeline already used for
 * font_tamzen_6x16 and font_siji_icons) - not part of the format itself.
 */
enum npb_font_id {
	NPB_FONT_TAMZEN_SMALL = 0,  /* 6x16px  - the existing menu font */
	NPB_FONT_TAMZEN_MEDIUM = 1, /* 8x16px  - native Tamzen size, no padding needed */
	NPB_FONT_TAMZEN_LARGE = 2,  /* 10x24px - native 10x20, bottom-padded to a multiple of 8 */
};

/* struct npb_row.flags bits. Bits 1-7 reserved, must be 0. */
#define NPB_ROW_FLAG_PARAGRAPH_START (1 << 0)

/*
 * One already-wrapped display row: `length` characters starting at
 * `offset` in the text blob. length == 0 marks a blank/paragraph-spacer
 * row - it carries no blob bytes, and by convention its offset is set to
 * the following content row's offset, so offsets stay monotonically
 * non-decreasing across the whole table (required for the binary-search
 * lookup described above).
 */
struct npb_row {
	uint32_t offset;
	uint8_t length;
	uint8_t flags;
	uint16_t reserved;
} __attribute__((packed));
/* 8 bytes */

/*
 * One font size's pre-paginated view of the book. glyph_width_px/
 * glyph_height_px/cols are redundant with what the firmware's own
 * compiled-in font table already knows for a given font_id - stored
 * anyway so a loader can sanity-check "this file was wrapped for a font
 * that doesn't match what's actually compiled in" instead of silently
 * rendering garbage.
 */
struct npb_variant_desc {
	uint8_t font_id; /* enum npb_font_id */
	uint8_t glyph_width_px;
	uint8_t glyph_height_px;
	uint8_t reserved0;
	uint16_t cols; /* columns this variant was wrapped for */
	uint16_t reserved1;
	uint32_t row_count;
	uint32_t row_table_offset; /* -> row_count x struct npb_row, absolute file offset */
	uint64_t reserved2;
} __attribute__((packed));
/* 24 bytes */

/*
 * One chapter, as pulled from the source EPUB's nav document/NCX.
 * text_offset is a byte offset into the shared text blob (NOT a
 * per-variant row index) - see the binary-search note above for why.
 */
struct npb_chapter {
	char title[64]; /* UTF-8, truncated to fit, NUL-padded */
	uint32_t text_offset;
	uint32_t reserved;
} __attribute__((packed));
/* 72 bytes */

/*
 * Fixed-size file header, always the first 256 bytes of a .npb file.
 * Every *_offset field below is an absolute byte offset from the start
 * of the file - section order on disk is otherwise unconstrained (a
 * converter would naturally emit header, variant table, one row table
 * per variant, chapter table, then the text blob last).
 */
struct npb_header {
	char magic[4]; /* NPB_MAGIC, "NPBK" */
	uint16_t version;
	uint16_t reserved0;
	char title[64];
	char author[64];
	uint32_t variant_count;
	uint32_t variant_table_offset; /* -> variant_count x struct npb_variant_desc */
	uint32_t chapter_count;
	uint32_t chapter_table_offset; /* -> chapter_count x struct npb_chapter, 0 if none */
	uint32_t text_blob_offset;
	uint32_t text_blob_size;
	uint8_t reserved1[96]; /* pads the struct to exactly 256 bytes */
} __attribute__((packed));
/* 256 bytes */

#endif /* APP_BOOK_NPB_FORMAT_H_ */
