/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOKMARKS_H_
#define APP_BOOKMARKS_H_

#include <stdbool.h>

/*
 * Bookmarks screen: lists the currently-open book's explicit bookmarks
 * (struct npb_bookmark, npb_bookmark_format.h - added/removed via the
 * Prev+Action combo while reading, see reader.h's top comment) via a
 * synthetic "Back" entry pinned at index 0 followed by each bookmark, in
 * the order they were added - same cursor/list convention as toc.c's
 * chapter list, including the synthetic-entry-at-index-0 idea.
 *
 * Each bookmark shows its page number (1-based - the page it would land
 * on if selected), computed by resolving the bookmark's blob offset
 * against the currently-open book's row table the same way toc.c
 * resolves a chapter's offset, then dividing by reader.h's
 * app_reader_get_content_rows() - plus, if the book has chapters, the
 * title of whichever one the bookmark falls in (a linear scan of the
 * same chapter table toc.c lists, finding the last chapter starting at
 * or before the bookmark's own offset). Reads reader.c's currently-open
 * book
 * (app_reader_get_path()/app_reader_get_font()) directly, same reasoning
 * as toc.c - this screen only ever makes sense while a book is open
 * (reachable exclusively via reader_menu.h).
 *
 * Loads the whole bookmark list (already resident in reader.c's own RAM,
 * see app_reader_get_bookmarks()) plus each entry's resolved page number
 * into a local array once, in app_bookmarks_show() - cursor moves and
 * scrolling afterward are pure in-memory list navigation.
 */

/*
 * Shows the bookmark list and takes over button input. Selecting "Back"
 * calls `on_back` - reader_menu.c's hook for redisplaying the in-book
 * menu this screen was opened from, same split (and same reasoning) as
 * toc.h's app_toc_show(). Selecting an actual bookmark instead
 * repositions the book there first (app_reader_jump_to_offset(),
 * reader.h - the same primitive toc.c uses for a chapter) and calls
 * `on_selected`, returning straight to reading.
 *
 * If the book has no bookmarks yet, this still shows correctly, just
 * with only the "Back" entry - not treated as an error.
 */
void app_bookmarks_show(void (*on_back)(void), void (*on_selected)(void));

/* For main.c's header-label priority chain - see app_toc_is_active()'s
 * identical purpose in toc.h.
 */
bool app_bookmarks_is_active(void);

#endif /* APP_BOOKMARKS_H_ */
