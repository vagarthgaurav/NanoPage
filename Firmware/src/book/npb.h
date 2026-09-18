/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_H_
#define APP_BOOK_NPB_H_

#include <stdint.h>
#include <zephyr/fs/fs.h>

#include "npb_format.h"
#include "text.h"

/*
 * .npb file I/O - mechanism only (opens/validates a file, reads a row-table
 * entry or a span of the text blob, binary-searches a blob offset to a row
 * index), no paging/UI/bookmark policy - that's reader.c, same
 * mechanism/policy split as max17055.c vs fuel_gauge.c and sd.c vs
 * browser.c elsewhere in this app. See docs/book-format.md and
 * npb_format.h for the on-disk format this reads.
 *
 * A book's row table can be tens of thousands of entries (~80KB for a
 * 90,000-word novel at the smallest font - see book-format.md's size sanity
 * check) - far too big to hold in RAM alongside everything else already
 * resident (fuel gauge, display/text.c framebuffers, NVS). So this never
 * loads the row table itself: every lookup is a fresh fs_seek()+fs_read()
 * of just the 8 bytes (or text span) actually needed, including during the
 * binary search used to resolve a saved position back to a row index -
 * O(log row_count) tiny reads, trivial against an already-open SD card.
 *
 * npb_open() picks whichever variant's recorded glyph_width_px/
 * glyph_height_px/cols matches the caller-specified `font`'s own compiled-in
 * dimensions (via text.c's app_text_font_info(), NOT the app's currently
 * active font/grid - reader.c always requests APP_TEXT_FONT_TAMZEN_MEDIUM
 * for book content regardless of what header.c/footer.c/browser.c are
 * using for the rest of the UI, see app_text_draw_row_font() in text.h for
 * why that's possible without disturbing the active grid). Fails with
 * -ENOTSUP if the file has no variant matching `font` at all - see
 * book-format.md's own reasoning for storing glyph dimensions redundantly
 * ("sanity-check instead of silently rendering garbage").
 */

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

/*
 * Opens `path` (a full fs path, mount point included - callers go through
 * sd.h's app_sd_full_path() first, npb.c has no SD-specific knowledge of
 * its own), and validates the header/variant table as described above
 * against `font`. Leaves the file open on success; call npb_close() when
 * done, including on a later error from any of the read/lookup functions
 * below. Returns 0 on success, negative errno otherwise.
 */
int npb_open(const char *path, enum app_text_font font, struct npb_book *book);

/* Closes the file. Safe to call on a book that failed to fully open. */
void npb_close(struct npb_book *book);

/*
 * Reads row table entry `row_index` (0-based) into `out`. Returns 0 on
 * success, -EINVAL if row_index >= book->row_count, negative errno on an
 * I/O failure.
 */
int npb_read_row(struct npb_book *book, uint32_t row_index, struct npb_row *out);

/*
 * Reads chapter table entry `chapter_index` (0-based) into `out` - see
 * npb_format.h's struct npb_chapter and docs/book-format.md's "Chapters"
 * section. Returns 0 on success, -EINVAL if chapter_index >=
 * book->chapter_count, negative errno on an I/O failure. A book with no
 * detected chapters (book->chapter_count == 0, e.g. the source EPUB had
 * no nav doc/NCX) always fails with -EINVAL here - not an error case
 * callers (toc.c) need to treat specially, just an empty table.
 */
int npb_read_chapter(struct npb_book *book, uint32_t chapter_index, struct npb_chapter *out);

/*
 * Reads `length` bytes starting at `blob_offset` (a byte offset into the
 * text blob, i.e. a struct npb_row's own `offset` field - NOT a file
 * offset) into caller-provided `buf`. Returns 0 on success, negative errno
 * on an I/O failure or if the span runs past the end of the blob.
 */
int npb_read_text(struct npb_book *book, uint32_t blob_offset, uint8_t length, char *buf);

/*
 * Binary-searches the row table for the row spanning `blob_offset` - the
 * primitive book-format.md describes for resolving a saved position,
 * bookmark, or chapter jump to a row index (see npb.h's own top comment for
 * why this reads the table via I/O rather than an in-memory search).
 * Finds the last row whose own offset is <= blob_offset (row offsets are
 * monotonically non-decreasing by construction - see npb_format.h), which
 * is always well-defined for any offset that's actually inside the blob.
 * Returns 0 and sets *row_index on success, negative errno on an I/O
 * failure or if book->row_count is 0.
 */
int npb_find_row(struct npb_book *book, uint32_t blob_offset, uint32_t *row_index);

#endif /* APP_BOOK_NPB_H_ */
