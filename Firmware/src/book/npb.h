/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_H_
#define APP_BOOK_NPB_H_

#include <stdint.h>
#include <zephyr/fs/fs.h>

#include "npb_format.h"
#include "text.h"

struct npb_book {
	struct fs_file_t file;
	uint32_t row_table_offset;
	uint32_t row_count;
	uint32_t text_blob_offset;
	uint32_t text_blob_size;
	uint32_t chapter_table_offset;
	uint32_t chapter_count;
	uint8_t font_id; /* enum npb_font_id - the variant that was matched */
};

int npb_open(const char *path, enum app_text_font font, struct npb_book *book);

void npb_close(struct npb_book *book);

int npb_read_row(struct npb_book *book, uint32_t row_index, struct npb_row *out);

int npb_read_chapter(struct npb_book *book, uint32_t chapter_index, struct npb_chapter *out);

int npb_read_text(struct npb_book *book, uint32_t blob_offset, uint8_t length, char *buf);

int npb_find_row(struct npb_book *book, uint32_t blob_offset, uint32_t *row_index);

#endif /* APP_BOOK_NPB_H_ */
