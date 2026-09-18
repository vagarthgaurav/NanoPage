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
 *
 * Also registers a long-press callback (buttons.h) while a book is open:
 * holding Action opens reader_menu.h's in-book menu (Table of Contents,
 * Bookmarks) instead of turning a page or closing the book - the three
 * physical buttons are otherwise fully committed to paging/closing, so
 * hold-duration is what stands in for a fourth button. See
 * app_reader_jump_to_offset() below for the primitive that lets that
 * menu (by way of toc.c) reposition an already-open book.
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
 * The full path (SD mount point included, e.g. "/SD:/books/foo.npb") of
 * the currently-open book - for toc.c to npb_open() its own, independent
 * handle on the same file to read its chapter table, without reaching
 * into reader.c's own book-open/close bracketing. Only valid while a
 * book is open (i.e. from within a long-press-triggered reader_menu/toc
 * session) - undefined otherwise.
 */
const char *app_reader_get_path(void);

/*
 * Repositions the currently-open book to the row containing
 * `text_offset` (a text-blob byte offset - a struct npb_chapter's own
 * text_offset field, or any other value the binary-search primitive
 * npb_find_row() resolves - see docs/book-format.md) and saves it to the
 * bookmark sidecar - toc.c's hook for "the user picked a chapter."
 *
 * Deliberately does NOT render or refresh the screen itself - toc.c
 * always calls this immediately before returning control via its own
 * on_exit chain, which lands back on reopen_reading() (reader.c), and
 * that already renders+full-refreshes unconditionally to repaint over
 * whatever the menu/toc screen was showing (needed even when nothing
 * was repositioned, e.g. the menu's plain "Back to Book"). Rendering
 * here too would pay that same ~1.8s full-refresh cost twice in a row
 * for nothing. If a future caller needs the new position visible
 * without going through that chain, it should render explicitly itself
 * rather than this function growing an implicit side effect only some
 * callers want.
 *
 * Returns 0 on success, -EINVAL if no book is open, negative errno
 * otherwise (position left unchanged on failure).
 */
int app_reader_jump_to_offset(uint32_t text_offset);

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
