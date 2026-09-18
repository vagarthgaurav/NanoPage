/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOK_NPB_BOOKMARK_FORMAT_H_
#define APP_BOOK_NPB_BOOKMARK_FORMAT_H_

#include <stdint.h>

/*
 * NanoPage Book bookmark sidecar (e.g. "/books/.nanopage/foo.npb.bmk" for
 * a book at "/books/foo.npb" - see reader.c's APP_READER_BMK_DIR) -
 * on-disk format only, no read/write logic here. See npb_format.h for the
 * companion .npb content format and docs/book-format.md for the full
 * spec.
 *
 * A .npb file is pure, immutable PC-side converter output. Bookmarks and
 * the last-read position are mutable, per-reader state, so they live in
 * a small separate sidecar file instead - re-converting a book (a fixed
 * typo, a new font size) never risks wiping a reader's saved places, and
 * the .npb file itself never needs a safe-to-mutate-on-flash story.
 *
 * Every position in this file - last_position and each bookmark's
 * text_offset - is a byte offset into the *same shared text blob* that
 * npb_format.h's struct npb_row/struct npb_chapter offsets refer to, not
 * a per-font row index. That's what lets a saved place survive switching
 * the active font size: re-open at the same blob offset, then
 * binary-search the newly-active variant's row table (see npb_format.h)
 * for the row that now contains it.
 *
 * Fixed-size, fixed-capacity (NPB_MAX_BOOKMARKS slots, no growable list)
 * so the whole file is small enough to rewrite wholesale in one fs_write
 * whenever a bookmark is added/removed or the reading position advances -
 * there's no partial-write/corruption case to reason about. Bookmark slot
 * management policy (fixed capacity vs. overwrite-oldest vs. user picks
 * which to delete, etc.) is reader-UI behavior, not format - deferred
 * along with the rest of the reader implementation.
 */

#define NPB_BOOKMARK_MAGIC   "NPBM" /* 4 bytes on disk, no NUL terminator */
#define NPB_BOOKMARK_VERSION 1
#define NPB_MAX_BOOKMARKS    16

/*
 * One user-created bookmark. label is an optional short user note -
 * empty (label[0] == '\0') means unlabeled, distinguished from
 * last_position below purely by being one of the explicit bookmark
 * slots the user asked to keep, versus the reader's own passive
 * auto-resume point.
 */
struct npb_bookmark {
	uint32_t text_offset;
	char label[32]; /* UTF-8, truncated to fit, NUL-padded; empty = unlabeled */
	uint32_t reserved;
} __attribute__((packed));
/* 40 bytes */

/*
 * Whole sidecar file, always read/written in one shot. last_position is
 * updated on every page turn (or at least on exiting the reader) and is
 * what "resume where I left off" reads on open - independent of, and
 * always present alongside, whatever explicit bookmarks the user has
 * saved in `bookmarks[0..bookmark_count)`.
 */
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
