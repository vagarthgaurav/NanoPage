/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_TOC_H_
#define APP_TOC_H_

#include <stdbool.h>

/*
 * Table-of-contents screen: lists the currently-open book's chapters
 * (struct npb_chapter, npb_format.h) via a synthetic "Back" entry pinned
 * at index 0 followed by every chapter title, in file order - same
 * cursor/scrolling list convention as browser.c's directory listing,
 * including the synthetic-entry-at-index-0 idea (browser.c's own ".."
 * to go up a directory).
 *
 * Reads reader.c's currently-open book (app_reader_get_path()/
 * app_reader_get_font()) directly rather than being handed a book
 * handle, since this screen only ever makes sense while a book is open
 * (reachable exclusively via reader_menu.h, itself only reachable from
 * reader.c's own long-press handler).
 *
 * Loads the whole chapter table into RAM once, in app_toc_show(), the
 * same way browser.c's load_dir() loads a directory listing - cursor
 * moves and scrolling afterward are pure in-memory list navigation, no
 * repeated SD/npb.c reads per keypress.
 */

/*
 * Shows the chapter list and takes over button input. Selecting "Back"
 * calls `on_back` - reader_menu.c's hook for redisplaying the in-book
 * menu this screen was opened from, so backing out of the chapter list
 * returns you to the menu, not all the way to the page. Selecting an
 * actual chapter instead repositions the book first
 * (app_reader_jump_to_offset(), reader.h) and calls `on_selected` -
 * reader_menu.c passes its own `on_exit` through for this one, the same
 * callback reader.c handed the menu in the first place, so picking a
 * chapter really does return straight to reading rather than back to
 * the menu.
 *
 * If the book has no chapters (npb_book.chapter_count == 0 - the source
 * EPUB had no nav doc/NCX) this still shows correctly, just with only
 * the "Back" entry - not treated as an error.
 */
void app_toc_show(void (*on_back)(void), void (*on_selected)(void));

/* For main.c's header-label priority chain - see
 * app_reader_menu_is_active()'s identical purpose in reader_menu.h. */
bool app_toc_is_active(void);

#endif /* APP_TOC_H_ */
