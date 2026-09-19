/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_FORMAT_H_
#define APP_BOOK_NPB_FORMAT_H_

#include <stdint.h>

#define NPB_MAGIC        "NPBK" /* 4 bytes on disk, no NUL terminator */
#define NPB_VERSION      1
#define NPB_MAX_VARIANTS 8 /* room to grow past the initial 3 sizes */

enum npb_font_id {
	NPB_FONT_TAMZEN_SMALL = 0,  /* 6x16px  - the existing menu font */
	NPB_FONT_TAMZEN_MEDIUM = 1, /* 8x16px  - native Tamzen size, no padding needed */
	NPB_FONT_TAMZEN_LARGE = 2,  /* 10x24px - native 10x20, bottom-padded to a multiple of 8 */
};

/* struct npb_row.flags bits. Bits 1-7 reserved, must be 0. */
#define NPB_ROW_FLAG_PARAGRAPH_START (1 << 0)

struct npb_row {
	uint32_t offset;
	uint8_t length;
	uint8_t flags;
	uint16_t reserved;
} __attribute__((packed));
/* 8 bytes */

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

struct npb_chapter {
	char title[64]; /* UTF-8, truncated to fit, NUL-padded */
	uint32_t text_offset;
	uint32_t reserved;
} __attribute__((packed));
/* 72 bytes */

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
