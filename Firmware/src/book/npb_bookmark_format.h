/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_BOOKMARK_FORMAT_H_
#define APP_BOOK_NPB_BOOKMARK_FORMAT_H_

#include <stdint.h>

#define NPB_BOOKMARK_MAGIC   "NPBM" /* 4 bytes on disk, no NUL terminator */
#define NPB_BOOKMARK_VERSION 1
#define NPB_MAX_BOOKMARKS    16

struct npb_bookmark {
	uint32_t text_offset;
	char label[32]; /* UTF-8, truncated to fit, NUL-padded; empty = unlabeled */
	uint32_t reserved;
} __attribute__((packed));
/* 40 bytes */

struct npb_bookmark_file {
	char magic[4]; /* NPB_BOOKMARK_MAGIC, "NPBM" */
	uint16_t version;
	uint8_t last_font_id; /* enum npb_font_id, from npb_format.h */
	uint8_t bookmark_count; /* 0..NPB_MAX_BOOKMARKS, how many of bookmarks[] are valid */
	uint32_t last_position; /* text blob byte offset - auto-resume point */
	struct npb_bookmark bookmarks[NPB_MAX_BOOKMARKS];
} __attribute__((packed));
/* 12 + 16 * 40 = 652 bytes */

#endif /* APP_BOOK_NPB_BOOKMARK_FORMAT_H_ */
