/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/kernel.h>

#include "npb.h"
#include "text.h"

static int read_at(struct npb_book *book, off_t offset, void *buf, size_t size)
{
	int ret;
	ssize_t n;

	ret = fs_seek(&book->file, offset, FS_SEEK_SET);
	if (ret) {
		return ret;
	}

	n = fs_read(&book->file, buf, size);
	if (n < 0) {
		return (int)n;
	}
	if ((size_t)n != size) {
		return -EIO;
	}

	return 0;
}

int npb_open(const char *path, enum app_text_font font, struct npb_book *book)
{
	struct npb_header header;
	struct npb_variant_desc variant;
	uint16_t cols, glyph_width, glyph_height;
	bool found = false;
	int ret;

	memset(book, 0, sizeof(*book));
	fs_file_t_init(&book->file);

	ret = fs_open(&book->file, path, FS_O_READ);
	if (ret) {
		printk("npb: open(%s) failed (%d)\n", path, ret);
		return ret;
	}

	ret = read_at(book, 0, &header, sizeof(header));
	if (ret) {
		printk("npb: failed to read header (%d)\n", ret);
		goto err;
	}

	if (memcmp(header.magic, NPB_MAGIC, sizeof(header.magic)) != 0) {
		printk("npb: bad magic\n");
		ret = -EINVAL;
		goto err;
	}
	if (header.version != NPB_VERSION) {
		printk("npb: unsupported version %u\n", header.version);
		ret = -ENOTSUP;
		goto err;
	}

	/* Match by `font`'s own glyph size/column count, not a hardcoded
	 * font_id and not whatever font happens to be globally active - see
	 * npb.h's top comment.
	 */
	app_text_font_info(font, &glyph_width, &glyph_height, &cols);

	for (uint32_t i = 0; i < header.variant_count && !found; i++) {
		ret = read_at(book, header.variant_table_offset + i * sizeof(variant), &variant,
			      sizeof(variant));
		if (ret) {
			printk("npb: failed to read variant %u (%d)\n", i, ret);
			goto err;
		}

		if (variant.glyph_width_px == glyph_width &&
		    variant.glyph_height_px == glyph_height && variant.cols == cols) {
			found = true;
		}
	}

	if (!found) {
		printk("npb: no variant matches font %d (%ux%u, %u cols)\n", font, glyph_width,
		       glyph_height, cols);
		ret = -ENOTSUP;
		goto err;
	}

	book->row_table_offset = variant.row_table_offset;
	book->row_count = variant.row_count;
	book->text_blob_offset = header.text_blob_offset;
	book->text_blob_size = header.text_blob_size;
	book->chapter_table_offset = header.chapter_table_offset;
	book->chapter_count = header.chapter_count;
	book->font_id = variant.font_id;

	return 0;

err:
	fs_close(&book->file);
	return ret;
}

void npb_close(struct npb_book *book)
{
	fs_close(&book->file);
}

int npb_read_row(struct npb_book *book, uint32_t row_index, struct npb_row *out)
{
	if (row_index >= book->row_count) {
		return -EINVAL;
	}

	return read_at(book, book->row_table_offset + (off_t)row_index * sizeof(*out), out,
		       sizeof(*out));
}

int npb_read_chapter(struct npb_book *book, uint32_t chapter_index, struct npb_chapter *out)
{
	if (chapter_index >= book->chapter_count) {
		return -EINVAL;
	}

	return read_at(book, book->chapter_table_offset + (off_t)chapter_index * sizeof(*out), out,
		       sizeof(*out));
}

int npb_read_text(struct npb_book *book, uint32_t blob_offset, uint8_t length, char *buf)
{
	if ((uint32_t)blob_offset + length > book->text_blob_size) {
		return -EINVAL;
	}

	return read_at(book, book->text_blob_offset + blob_offset, buf, length);
}

int npb_find_row(struct npb_book *book, uint32_t blob_offset, uint32_t *row_index)
{
	uint32_t lo, hi, result;
	bool have_result = false;
	int ret;

	if (book->row_count == 0) {
		return -EINVAL;
	}

	lo = 0;
	hi = book->row_count - 1;

	while (lo <= hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		struct npb_row row;

		ret = npb_read_row(book, mid, &row);
		if (ret) {
			return ret;
		}

		if (row.offset <= blob_offset) {
			result = mid;
			have_result = true;
			lo = mid + 1;
		} else {
			if (mid == 0) {
				break;
			}
			hi = mid - 1;
		}
	}

	if (!have_result) {
		result = 0;
	}

	*row_index = result;
	return 0;
}
