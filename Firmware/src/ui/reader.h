/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_READER_H_
#define APP_READER_H_

#include "text.h"

/*
 * Book reader screen - a structural sibling of browser.c (see
 * docs/book-format.md's "next planned step"). Renders a .npb file's
 * already-wrapped rows into text.c's grid, between the header (row 0,
 * header.h) and footer (the grid's last row, footer.h) - same content-area
 * convention browser.c uses, so opening a book never disturbs the
 * clock/battery chrome either bar already shows. Registers itself as
 * buttons.h's press callback while active: Prev/Next turn the page
 * backward/forward, Action closes the book and hands control back to
 * whoever opened it (browser.c, via the on_exit callback below) - the only
 * three physical buttons this board has are already spoken for by paging,
 * so closing reuses Action rather than needing a fourth, nonexistent
 * button.
 *
 * Book content defaults to APP_TEXT_FONT_TAMZEN (the smallest size,
 * matching the rest of the UI) - see app_reader_set_font() below for
 * changing it, independent of whatever font header.c/footer.c/browser.c
 * use. Auto-resumes from the book's <path>.npb.bmk sidecar
 * (npb_bookmark_format.h) if one exists, starting from the beginning
 * otherwise; the sidecar's last_position is rewritten on every page turn
 * and again on close, so resume position survives a reboot or a plain
 * reflash.
 *
 * Refresh policy mirrors browser.c: a page turn always redraws every
 * visible content row (a page turn is inherently a full-content change,
 * unlike a browser cursor move) through app_text_flush(), except every
 * APP_READER_FULL_REFRESH_EVERY-th one, which goes through
 * app_text_full_refresh() instead to clear partial-refresh ghosting -
 * same reasoning and same per-module counter convention as
 * APP_BROWSER_FULL_REFRESH_EVERY in browser.c.
 */

/*
 * Opens `path` (SD-root-relative, e.g. "/books/foo.npb" - same convention
 * browser.c's current_path uses) as a book and takes over button input.
 * `on_exit` is called once, from the Action-button handler, after the
 * bookmark has been saved and the file closed - the caller (browser.c) is
 * expected to use it to re-register its own button callback and redraw its
 * listing, the same way app_reader_open() itself only returns after the
 * initial page is drawn and flushed.
 *
 * Returns 0 on success, negative errno if the path couldn't be opened, the
 * file isn't a recognized/supported .npb (see npb.h), or the screen is too
 * short for any content rows. On failure, button input is left exactly as
 * it was before this call - the caller's own callback is untouched.
 */
int app_reader_open(const char *path, void (*on_exit)(void));

/* The font book content currently renders in - for a future settings menu
 * to read back and highlight the active choice.
 */
enum app_text_font app_reader_get_font(void);

/*
 * Changes the font book content renders in - the hook a future settings
 * menu calls (no menu exists yet; nothing in this codebase calls this
 * today). Only APP_TEXT_FONT_TAMZEN/_MEDIUM/_LARGE make sense here (the
 * three sizes npb_format.h's enum npb_font_id defines) - any other value
 * will simply fail to match a variant in every book, per npb_open().
 *
 * If a book is currently open, re-opens it at the new font, carrying the
 * current reading position over (resolved through the same blob-offset
 * binary search app-format.md describes for a saved bookmark - see
 * npb_find_row()), and redraws immediately. If the new font has no
 * matching variant in the currently-open book, reverts to the previous
 * font rather than leaving the reader broken. If no book is open, just
 * changes the font app_reader_open() will use next.
 */
void app_reader_set_font(enum app_text_font font);

#endif /* APP_READER_H_ */
