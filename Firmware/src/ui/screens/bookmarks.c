/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "bookmarks.h"
#include "buttons.h"
#include "config.h"
#include "header.h"
#include "npb.h"
#include "npb_bookmark_format.h"
#include "reader.h"
#include "sd.h"
#include "text.h"

/* Row layout - same cursor/name convention as toc.c/reader_menu.c (no
 * per-entry type icon; every entry here is either the synthetic "Back"
 * or a bookmark, and neither needs one).
 */
#define APP_BOOKMARKS_CURSOR_COL 0
#define APP_BOOKMARKS_NAME_COL   3

/* Bookmarks + the synthetic "Back" entry pinned at index 0 - fits in one
 * static array with no dynamic allocation, same reasoning as toc.c's
 * APP_TOC_MAX_ENTRIES (NPB_MAX_BOOKMARKS is already a hard cap on how
 * many bookmarks can exist at all, so there's no "silently drop the
 * tail" case here the way there is for an arbitrarily long chapter
 * table or directory listing).
 */
#define APP_BOOKMARKS_MAX_ENTRIES (NPB_MAX_BOOKMARKS + 1)

/* "<label> - Page <N>", "<chapter title> - Page <N>", or plain "Page <N>" -
 * see load_bookmarks()'s own comment on why the label/chapter title are
 * each copied into a local NUL-terminated buffer first rather than
 * formatted directly from struct npb_bookmark/struct npb_chapter. Sized
 * for the worst case (a full-length label or chapter title, both capped
 * at 64 chars - see npb_bookmark_format.h/npb_format.h - plus the " - Page
 * NNNNN" suffix) rather than tightly, since the extra bytes are cheap and
 * draw_name() only ever displays what actually fits the screen's own
 * column count regardless of how much backing text exists.
 */
#define APP_BOOKMARKS_TEXT_LEN 96

/* Matches struct npb_chapter.title (npb_format.h) - see toc.c's identical
 * APP_TOC_TITLE_LEN for why the +1 below is needed (NUL-padded on disk,
 * not guaranteed NUL-terminated if every byte is real title content).
 */
#define APP_BOOKMARKS_CHAPTER_TITLE_LEN 64

struct bookmarks_entry {
	char text[APP_BOOKMARKS_TEXT_LEN];
	uint32_t text_offset;
	bool is_back;
};

static struct bookmarks_entry entries[APP_BOOKMARKS_MAX_ENTRIES];
static size_t entry_count;
static size_t selected_index;
static size_t scroll_offset;

/* Set once per app_bookmarks_show() call - see bookmarks.h's own doc
 * comment on the two-callback split: on_back_cb returns to the in-book
 * menu ("Back"), on_selected_cb returns straight to reading (an actual
 * bookmark jump).
 */
static void (*on_back_cb)(void);
static void (*on_selected_cb)(void);

static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static uint8_t partial_count;
static bool bookmarks_active;

static void on_button(enum app_button button);

/*
 * Finds the chapter containing `offset` in `book`'s chapter table (the
 * same idea npb_find_row() implements for the row table, but there's no
 * shared primitive for chapters - see npb.h's own top comment on why
 * npb.c doesn't load either table into RAM, only reads entries on
 * demand) - the last chapter whose own text_offset is <= `offset`,
 * relying on struct npb_chapter's documented "in file order" placement
 * (book-format.md's "Chapters" section - pulled from the source EPUB's
 * nav doc/NCX in reading order, so text_offset is monotonically
 * non-decreasing across the table the same way a row table's is). A
 * plain linear scan rather than a binary search: book->chapter_count is
 * already capped at APP_TOC_MAX_CHAPTERS-scale (a few dozen) by
 * whatever produced it, and this only runs once per bookmark when the
 * list screen opens, not per keypress.
 *
 * Returns true and fills `out` (NUL-terminated, truncated to fit) if
 * `offset` falls at or after at least one chapter's start; false if the
 * book has no chapters at all, or `offset` is somehow before the first
 * one (out left untouched either way).
 */
static bool chapter_title_for_offset(struct npb_book *book, uint32_t offset, char *out,
				      size_t out_size)
{
	bool found = false;

	for (uint32_t i = 0; i < book->chapter_count; i++) {
		struct npb_chapter chapter;

		if (npb_read_chapter(book, i, &chapter)) {
			break;
		}

		if (chapter.text_offset > offset) {
			/* Every later chapter starts even further in - none
			 * of them can be the one containing `offset` either.
			 */
			break;
		}

		strncpy(out, chapter.title, out_size - 1);
		out[out_size - 1] = '\0';
		found = true;
	}

	return found;
}

/*
 * Loads the currently-open book's bookmarks into `entries`, formatting
 * each one's display text with its resolved chapter title and page
 * number. Bookmark data itself comes from reader.c's own in-memory copy
 * (app_reader_get_bookmarks() - no SD access needed for that part, see
 * reader.h), but resolving a blob offset to a page number/chapter needs
 * the book's row/chapter tables, which means a fresh npb_open() of its
 * own - same mechanism/independent-handle reasoning as toc.c's
 * load_chapters(). Brackets its own app_sd_init()/app_sd_deinit(). A
 * bookmark whose page number/chapter couldn't be resolved (SD
 * unavailable, or some other failure) still gets a list entry, just
 * without that detail in its text - not fatal to showing the rest of
 * the list.
 */
static int load_bookmarks(void)
{
	struct npb_bookmark raw[NPB_MAX_BOOKMARKS];
	struct npb_book book;
	size_t raw_count = 0;
	uint16_t content_rows_per_page = app_reader_get_content_rows();
	bool book_opened = false;

	strncpy(entries[0].text, "Back", sizeof(entries[0].text) - 1);
	entries[0].text[sizeof(entries[0].text) - 1] = '\0';
	entries[0].is_back = true;
	entries[0].text_offset = 0;
	entry_count = 1;

	if (app_reader_get_bookmarks(raw, ARRAY_SIZE(raw), &raw_count) || raw_count == 0) {
		selected_index = 0;
		scroll_offset = 0;
		return 0;
	}

	if (app_sd_init() == 0) {
		book_opened = (npb_open(app_reader_get_path(), app_reader_get_font(), &book) == 0);
		if (!book_opened) {
			app_sd_deinit();
		}
	}

	for (size_t i = 0; i < raw_count && entry_count < APP_BOOKMARKS_MAX_ENTRIES; i++) {
		/* struct npb_bookmark.label is "truncated to fit, NUL-padded" -
		 * not guaranteed NUL-terminated if every byte is real label
		 * content (same wording, same caveat, as struct npb_chapter.
		 * title, handled the same way inside chapter_title_for_offset()
		 * above) - copy into a local buffer with room for an explicit
		 * terminator before ever treating it as a C string.
		 */
		char label[sizeof(raw[i].label) + 1];
		char chapter_title[APP_BOOKMARKS_CHAPTER_TITLE_LEN + 1];
		uint32_t row_index;
		uint32_t page_number = 0;
		bool have_chapter = false;
		/* Whichever of label/chapter title is more specific for this
		 * bookmark - label wins when set (nothing sets one today, but
		 * the sidecar format already has the field - see npb_bookmark_
		 * format.h), otherwise the chapter it falls in, otherwise
		 * neither.
		 */
		const char *prefix = NULL;

		memcpy(label, raw[i].label, sizeof(raw[i].label));
		label[sizeof(raw[i].label)] = '\0';

		if (book_opened) {
			if (content_rows_per_page > 0 &&
			    npb_find_row(&book, raw[i].text_offset, &row_index) == 0) {
				page_number = row_index / content_rows_per_page + 1;
			}

			have_chapter = chapter_title_for_offset(&book, raw[i].text_offset,
								 chapter_title,
								 sizeof(chapter_title));
		}

		if (label[0] != '\0') {
			prefix = label;
		} else if (have_chapter) {
			prefix = chapter_title;
		}

		if (prefix && page_number != 0) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "%s - Page %u", prefix, page_number);
		} else if (prefix) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "%s", prefix);
		} else if (page_number != 0) {
			snprintf(entries[entry_count].text, sizeof(entries[entry_count].text),
				 "Page %u", page_number);
		} else {
			strncpy(entries[entry_count].text, "Bookmark",
				sizeof(entries[entry_count].text) - 1);
			entries[entry_count].text[sizeof(entries[entry_count].text) - 1] = '\0';
		}

		entries[entry_count].text_offset = raw[i].text_offset;
		entries[entry_count].is_back = false;
		entry_count++;
	}

	if (book_opened) {
		npb_close(&book);
		app_sd_deinit();
	}

	selected_index = 0;
	scroll_offset = 0;

	return 0;
}

static void draw_name(uint16_t row, const char *name)
{
	size_t len = strlen(name);

	for (uint16_t col = APP_BOOKMARKS_NAME_COL; col < grid_cols; col++) {
		size_t i = col - APP_BOOKMARKS_NAME_COL;

		app_text_draw_cell(row, col, (i < len) ? name[i] : ' ');
	}
}

/* `page_row` is 0-based within the visible content area - see browser.c's
 * identical convention for why (keeps scroll_offset math in one place).
 */
static void draw_entry(uint16_t page_row, size_t idx)
{
	uint16_t row = content_start_row + page_row;

	app_text_draw_row(row, "");

	if (idx >= entry_count) {
		return;
	}

	draw_name(row, entries[idx].text);
}

static void draw_cursor(uint16_t page_row, bool show)
{
	uint16_t row = content_start_row + page_row;

	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(APP_BOOKMARKS_CURSOR_COL),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, APP_BOOKMARKS_CURSOR_COL, ' ');
		app_text_draw_cell(row, APP_BOOKMARKS_CURSOR_COL + 1, ' ');
	}
}

static void redraw_page(void)
{
	for (uint16_t i = 0; i < content_rows; i++) {
		draw_entry(i, scroll_offset + i);
	}

	if (entry_count > 0) {
		draw_cursor((uint16_t)(selected_index - scroll_offset), true);
	}
}

static void bookmarks_flush(void)
{
	partial_count++;

	if (partial_count >= APP_BOOKMARKS_FULL_REFRESH_EVERY) {
		app_text_full_refresh();
		partial_count = 0;
	} else {
		app_text_flush();
	}
}

static void move_cursor(int delta)
{
	size_t new_index;
	uint16_t old_page_row;

	if (entry_count == 0) {
		return;
	}

	if (delta < 0) {
		if (selected_index == 0) {
			return;
		}
		new_index = selected_index - 1;
	} else {
		if (selected_index + 1 >= entry_count) {
			return;
		}
		new_index = selected_index + 1;
	}

	if (new_index < scroll_offset || new_index >= scroll_offset + content_rows) {
		scroll_offset = (new_index < scroll_offset) ? new_index
							      : new_index - content_rows + 1;
		selected_index = new_index;
		redraw_page();
	} else {
		old_page_row = (uint16_t)(selected_index - scroll_offset);
		draw_cursor(old_page_row, false);
		selected_index = new_index;
		draw_cursor((uint16_t)(selected_index - scroll_offset), true);
	}

	bookmarks_flush();
}

static void select_entry(void)
{
	bool is_back;
	void (*cb)(void);

	if (entry_count == 0) {
		return;
	}

	is_back = entries[selected_index].is_back;

	if (!is_back) {
		int ret = app_reader_jump_to_offset(entries[selected_index].text_offset);

		if (ret) {
			printk("bookmarks: failed to jump to bookmark (%d)\n", ret);
			/* Position unchanged (see app_reader_jump_to_offset()'s
			 * own doc comment) - still leave the screen via the
			 * same exit path rather than stranding the user here.
			 */
		}
	}

	cb = is_back ? on_back_cb : on_selected_cb;

	bookmarks_active = false;
	app_buttons_set_press_callback(NULL);

	if (cb) {
		cb();
	}
}

static void on_button(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		move_cursor(-1);
		break;
	case APP_BUTTON_NEXT:
		move_cursor(1);
		break;
	case APP_BUTTON_ACTION:
		select_entry();
		break;
	}
}

void app_bookmarks_show(void (*on_back)(void), void (*on_selected)(void))
{
	uint16_t rows;

	on_back_cb = on_back;
	on_selected_cb = on_selected;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	/* A failed load still shows the screen with just "Back" (entries[0]
	 * is always set before load_bookmarks() can fail) rather than
	 * leaving the user stuck with no button callback at all.
	 */
	load_bookmarks();

	redraw_page();
	app_header_update("Bookmarks", false);

	/* Always a full refresh - opening this screen is an infrequent,
	 * deliberate transition, same reasoning as toc.c's own
	 * app_toc_show().
	 */
	app_text_full_refresh();
	partial_count = 0;

	app_buttons_set_press_callback(on_button);
	bookmarks_active = true;
}

bool app_bookmarks_is_active(void)
{
	return bookmarks_active;
}
