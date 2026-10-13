# NanoPage Book format (.npb)

Status: **format designed, not yet implemented.** No converter and no
on-device reader exist yet — see `.claude/CLAUDE.md`'s "Book format
decision" note and the "next planned step" entries around the file
browser for where this fits. This document is the spec a future session
implements against.

## Why not EPUB

EPUB is a zip of XHTML/CSS. Parsing that on-device costs more RAM and CPU
than this board's battery-life target can afford — the same reasoning
that already ruled out grayscale display modes and proportional book
fonts elsewhere in this firmware. The fix: do all the expensive work
*once*, offline, on a PC, and ship the device a format it can render by
doing nothing but indexing into a table.

Source books are ordinary **EPUB files** the user already owns. A
PC-side converter (not built yet, expected to be Python — the project's
own font-conversion pipeline already is, and libraries like `ebooklib`/
`BeautifulSoup` make EPUB unzip/HTML-strip straightforward) turns each
into a `.npb` file dropped onto the SD card. The device never sees the
original EPUB or its markup.

## Design in one sentence

Word-wrapping happens once, offline, per font size; the device just
walks a table of `(offset, length)` spans into a shared plain-text blob.

Two things fall out of that:

- **Page turn = pure array-index math.** Advance the current row index by
  however many content rows the active screen layout has, look up that
  many row-table entries, `fs_seek`+`fs_read` those bytes, draw each with
  the existing `app_text_draw_row()` (`Firmware/src/ui/text.c`). No
  line-wrap/reflow math ever runs on-device.
- **Columns are fixed per font at conversion time** (the 296px panel
  width never changes), but **rows-per-page is a device-side runtime
  choice**, never stored in the file. Whether the reader keeps a
  header/footer visible or goes full-screen is a UX decision for later —
  changing it should never require re-converting every book.

## Fonts

Three fixed Tamzen sizes (not the built-in CFB SMALL/MEDIUM/LARGE fonts,
for a look consistent with the rest of the UI):

| `enum npb_font_id`       | Glyph size | Cols (296px / glyph width) | Notes |
|--------------------------|-----------:|---------------------------:|-------|
| `NPB_FONT_TAMZEN_SMALL`  | 6×16px     | 49 | The existing menu font, reused as-is |
| `NPB_FONT_TAMZEN_MEDIUM` | 8×16px     | 37 | Tamzen's native 8×16 bitmap — already a multiple of 8 tall, no padding needed |
| `NPB_FONT_TAMZEN_LARGE`  | 10×24px    | 29 | Tamzen's native 10×20 bitmap, bottom-padded to 24px (same technique already used for the 6×16 font) |

Cutting the actual `font_tamzen_8x16`/`font_tamzen_10x24` glyph bitmaps
(BDF→CFB conversion, same pipeline already used for `font_tamzen_6x16`
and `font_icons`) is separate font/firmware work, not part of this
format. Each variant descriptor in the file records its real glyph
width/height so a future loader can sanity-check its compiled-in font
table against what a given `.npb` file was actually wrapped for, instead
of silently rendering garbage on a mismatch.

## Character set

The text blob is 7-bit ASCII, printable range 32–126 — the exact range
Zephyr's CFB fonts (including `font_tamzen_6x16`, `first_char=32,
last_char=126`) have glyphs for. The converter transliterates common
Unicode punctuation and Latin characters it encounters in the EPUB's
markup (curly quotes → straight, em/en dash → `--`/`-`, ellipsis → `...`,
accented Latin letters → their unaccented ASCII form) and replaces
anything left un-transliterable with `?`, logging a count of substitutions
made so a conversion can be checked for how lossy it was.

Images are **out of scope for v1** — this is a 1-bit text-grid renderer,
not a general document viewer. An EPUB's cover/illustrations are simply
dropped during conversion.

## File layout

Single binary file per book, extension `.npb`. Struct definitions live in
`Firmware/src/book/npb_format.h` (the single source of truth — this
document explains them, it doesn't duplicate the canonical field list).
All multi-byte integers are **little-endian**, matching the nRF52840
natively. Every `*_offset` field is an absolute byte offset from the
start of the file, so sections can be laid out in any order; a converter
would naturally emit them in this order:

```
+----------------------------+  offset 0
| struct npb_header          |  256 bytes, fixed
+----------------------------+
| npb_variant_desc[N]        |  one per font size (N = variant_count)
+----------------------------+
| npb_row[] for variant 0    |
| npb_row[] for variant 1    |
| ...                        |
+----------------------------+
| npb_chapter[]              |  optional, chapter_count may be 0
+----------------------------+
| text blob                  |  flat ASCII, no delimiters
+----------------------------+
```

### Text blob

A flat run of ASCII printable characters with **no embedded delimiters**.
Every row's exact content is fully described by its own `(offset,
length)` pair — wrap points, paragraph breaks, and chapter starts never
need to be located by scanning for a newline or any other marker. A
"blank row" (`length == 0`, used for paragraph spacing) is pure row-table
metadata: it consumes no blob bytes, and by convention its `offset` is
set to the following content row's offset, so offsets stay monotonically
non-decreasing across a variant's whole row table.

Because there's no per-row delimiter, **every font variant's row table
references the same shared blob** — wrapping differently just means
choosing different `(offset, length)` windows over one underlying
character stream, not storing a separate copy of the text per font size.
The blob is stored once regardless of how many font sizes are
pre-paginated.

### Chapters

Pulled from the source EPUB's nav document (EPUB3) or NCX (EPUB2) during
conversion. Each `struct npb_chapter` records a title and a **text blob
byte offset** — not a per-font row index — because the same offset must
resolve correctly no matter which font variant is currently active.

### Resuming a position / jumping to a chapter or bookmark

Given any text blob byte offset — from a saved auto-resume position, an
explicit user bookmark (see below), or a chapter's `text_offset` — binary
search the *currently active* font variant's row table by `offset` for
the row that contains it (offsets are monotonically non-decreasing by
construction, so this is a standard sorted-array binary search, O(log
rows)). This one primitive covers all three cases identically, and is
what makes switching font size mid-book preserve your place: re-resolve
the same blob offset against the newly-active variant's row table.

This is the only non-trivial runtime operation this format implies —
everything else (page turn, forward/backward navigation) is direct array
indexing.

## Bookmarks (sidecar file)

Struct definitions live in `Firmware/src/book/npb_bookmark_format.h`.

A `.npb` file is pure, immutable converter output. Bookmarks and the
last-read position are mutable, per-reader state, so they're kept
**out** of the `.npb` file and instead live in a small companion sidecar,
`<book>.npb.bmk`, owned entirely by the device. The sidecar lives in a
hidden `.nanopage` subfolder of the book's own directory rather than
directly alongside it — e.g. a book at `/books/foo.npb` gets a sidecar at
`/books/.nanopage/foo.npb.bmk` — so it doesn't clutter the file browser's
listing with per-book bookkeeping the reader has no reason to see or
delete (the on-device browser already omits any dot-prefixed entry from
its listings). This means re-converting a book (a fixed typo, an added
font size) never risks wiping a reader's saved places, and the read-only
`.npb` file never needs a safe-to-mutate-on-flash story.

The sidecar holds:

- `last_position` — a text blob byte offset, updated on every page turn
  (or at least on exiting the reader), read back on open for "resume
  where I left off."
- up to `NPB_MAX_BOOKMARKS` (16) explicit user bookmarks, each a text
  blob byte offset plus an optional short label.
- `last_font_id` — which font size was active, for resume convenience.

Fixed capacity, no growable list, so the whole sidecar (652 bytes at 16
slots) is small enough to rewrite wholesale in one write whenever a
bookmark is added/removed or the position advances — there's no
partial-write corruption case to reason about. Every position in the
sidecar is a text blob offset, the same coordinate space `npb_row`/
`npb_chapter` offsets use, resolved via the same binary-search primitive
described above — so a bookmark survives a font-size switch exactly like
the auto-resume position and chapter jumps do.

Bookmark slot management policy is reader-UI behavior, not format —
implemented in `reader.c` (see "What's implemented so far" below): a
bookmark is keyed by its exact page-start blob offset, added or removed
by the Prev+Action button combo (fixed capacity, once full a new
bookmark is simply rejected rather than overwriting the oldest or
picking one to evict). Per-bookmark labels are part of the on-disk
format (`struct npb_bookmark.label`) but nothing currently sets one —
every bookmark added on-device today is unlabeled.

## Size sanity check

A ~90,000-word novel (~500KB of text) wraps to roughly 10,000 rows at 49
columns (smallest font). Per variant: `10,000 × 8 bytes ≈ 80KB` row
table. Three variants: ~240KB of row tables + ~500KB shared blob ≈ 750KB
per book, plus a negligible chapter table. Trivial for an SD card — file
size was never a constraint here, which is what lets the
row-table-per-character-span design stay simple instead of needing any
compression.

## What's implemented so far

- `Firmware/src/book/npb_format.h` — `npb_header`, `npb_variant_desc`,
  `npb_row`, `npb_chapter`, `enum npb_font_id`.
- `Firmware/src/book/npb_bookmark_format.h` — `npb_bookmark_file`,
  `npb_bookmark`.
- `Firmware/src/book/npb.c`/`.h` — mechanism-only `.npb` file I/O (opens
  and validates a file against a caller-specified font, reads row-table
  entries and text-blob spans on demand rather than loading the row table
  into RAM, and binary-searches a blob offset to a row index). No policy
  of its own — see `reader.c` below.
- `Firmware/src/ui/reader.c`/`.h` — the on-device reader screen, a
  structural sibling of `browser.c` in the same directory: opens a `.npb`
  file the browser hands off to it (`app_browser_init()`'s listing calls
  `app_reader_open()` for any selected `.npb` file), takes over Prev/Next
  for page turns and Action to close back to the browser, and reads/writes
  the `.npb.bmk` sidecar (in its hidden `.nanopage` subfolder, see above)
  for auto-resume. Book content defaults to the
  smallest font (`APP_TEXT_FONT_TAMZEN`) independent of whatever font the
  header/footer/browser are using, via `text.c`'s `app_text_draw_text_px()`
  (pixel-addressed, not tied to the active font's row grid — needed since
  `APP_TEXT_FONT_TAMZEN_LARGE`'s 24px lines don't share a row boundary with
  the header/footer's 16px rows). `app_reader_set_font()` is a ready hook
  for a future settings menu (none exists yet) to change the size and
  reflow the currently-open book in place.
- `Firmware/Converter/epub2npb.py` (plus `epub_reader.py`/`npb_struct.py`/
  `transliterate.py`/`wrap.py`) — the PC-side EPUB → `.npb` converter.
- `Firmware/src/ui/fonts/font_tamzen.c`/`.h` — all three Tamzen sizes
  (`font_tamzen_6x16`/`_8x16`/`_10x24`), converted from the upstream BDF
  glyphs (`sunaku/tamzen-font`) and independently verified before ever
  flashing by rendering the generated arrays back through `text.c`'s exact
  bit convention and comparing pixel-for-pixel against the source BDF.

- **Chapter navigation/jump**, added 2026-09-18. The Prev/Next/Action
  button-mapping conflict noted above (all three already committed to
  paging/closing while a book is open) is solved with a hold gesture
  instead of a fourth button: `nanopage.dts` wraps the Action button in
  Zephyr's in-tree `zephyr,input-longpress` pseudo-device
  (`subsys/input/input_longpress.c`), which re-emits either the same
  code on a short press (< 500ms - what `buttons.c` now treats as the
  real Action press everywhere, not the raw gpio-keys event, so every
  screen's Action now dispatches on release rather than press) or a
  distinct `INPUT_KEY_MENU` code on a long press (>= 500ms). `reader.c`
  registers a long-press callback (`buttons.h`'s
  `app_buttons_set_long_press_callback()`) only while a book is open,
  opening `Firmware/src/ui/reader_menu.c`/`.h` - an in-book menu (same
  cursor/list convention as `menu.c`/`browser.c`) with three entries:
  "Back to Book", "Table of Contents", and "Bookmarks" (present but
  currently a no-op selection - reserved for the explicit-bookmark-slot
  work below, not yet implemented). "Table of Contents" opens
  `Firmware/src/ui/toc.c`/`.h`, which loads the open book's
  `struct npb_chapter` table (via new `npb_read_chapter()` in
  `npb.c`/`.h`, and `struct npb_book` gaining `chapter_table_offset`/
  `chapter_count`) into a scrollable list (synthetic "Back" entry pinned
  at index 0, same idea as `browser.c`'s ".."), same
  cursor/scroll-page/full-refresh-every-N-moves convention as
  `browser.c`'s directory listing. Selecting a chapter calls new
  `app_reader_jump_to_offset()` in `reader.c` (resolves the chapter's
  `text_offset` to a row via the existing `npb_find_row()` binary search,
  updates position, saves the bookmark sidecar) and returns straight to
  reading - deliberately does *not* render/refresh itself, since the
  return path (`reader.c`'s `reopen_reading()`) always repaints
  unconditionally anyway (needed even for the menu's plain "Back to
  Book"); doing both would pay the ~1.8s full-refresh cost twice per
  jump. `main.c`'s header-label priority chain gained
  `app_reader_menu_is_active()`/`app_toc_is_active()` branches so the
  once-a-minute clock tick doesn't stomp the header back to the
  browser's last path while either screen is up. Verified on real
  hardware 2026-09-18: clean build/flash/boot (RTT capture confirmed the
  short-press filtering - a raw `dev=buttons` Action press+release
  followed by the corresponding `dev=longpress` echo, correctly
  dispatching exactly once); the long-press-to-menu gesture and TOC
  jump itself still need a live physical hold-Action test, not yet
  captured.

- **Explicit bookmark slots**, added 2026-09-18. Fills in `NPB_MAX_BOOKMARKS`
  (the reader menu's "Bookmarks" entry above was wired for this but
  inert until now). Two pieces:
  - **The Prev+Action combo**, `buttons.c`'s fourth gesture (alongside
    the long-press above) - holding Prev and Action together toggles a
    bookmark at the current page. New `buttons.h` API,
    `app_buttons_set_combo_callback()`: `buttons.c` tracks raw hold-state
    for Prev/Action only while a combo callback is registered, and fires
    it the instant the second button goes down while the first is
    already held. This has one real, deliberate side effect: while
    registered, Prev's own dispatch moves from press-triggered to
    release-triggered (matching Action's existing longpress-echo-on-
    release dispatch) - unavoidable, since whether a given Prev press
    turns out to be solo or part of a chord can't be known until either
    it's released alone or Action goes down while it's still held.
    Detection uses one-shot suppression flags (set the instant the combo
    fires, each consumed independently by its own button's next release/
    echo) rather than a persistent "combo active" flag, specifically to
    avoid a reset-timing race against the longpress device's own
    asynchronously re-queued short-press echo event. `reader.c` registers
    this callback only while an actual page is on screen (cleared
    whenever reader_menu.c/toc.c/bookmarks.c take over button input,
    re-registered on returning via `reopen_reading()`).
  - **The bookmark itself**: `reader.c` now holds the whole bookmark
    sidecar (`struct npb_bookmark_file`) resident in RAM for the reading
    session (loaded once by `load_bookmark()` on first open, not
    reloaded on a font switch - bookmarks survive that the same way
    `last_position` already did), instead of the old `save_bookmark()`
    that silently zeroed `bookmark_count` on every write. Toggling
    (`toggle_bookmark()`) does a fresh, short-lived `npb_open()` just to
    read the current page's blob offset, then adds or removes a slot
    with plain in-memory array surgery before `save_bookmark()` writes
    the whole struct back wholesale, per the sidecar's own fixed-size
    "always rewritten in one shot" design. A **bookmark icon**
    (`font_icons.h`'s new `APP_TEXT_ICON_BOOKMARK`, hand-drawn - a ribbon
    shape tapering to a V-notch, same geometric-icon approach as the
    existing settings "sliders" glyph, no source font for a bookmark
    glyph) takes over the header's folder-name spot (`header.h`'s
    `app_header_update()` gained a `bookmarked` bool parameter) whenever
    the currently-displayed page has a bookmark - reader.c always passes
    an empty folder alongside it, since no folder path is meaningful
    while reading. `main.c`'s header-label priority chain gained
    `app_reader_is_active()`/`app_bookmarks_is_active()` branches for
    this. The reader menu's "Bookmarks" entry now opens
    `Firmware/src/ui/screens/bookmarks.c`/`.h` - a scrollable list (same
    cursor/synthetic-"Back"-at-index-0 convention as `toc.c`) showing
    each bookmark's **page number**, resolved by binary-searching its
    blob offset against the book's row table (same primitive `toc.c`
    uses for a chapter) and dividing by a new `app_reader_get_content_rows()`
    getter so the page-number math isn't duplicated outside reader.c.
    Selecting a bookmark reuses `app_reader_jump_to_offset()` unchanged -
    jumping to a saved place is the same operation regardless of whether
    the offset came from a chapter or a bookmark. Builds clean; not yet
    verified on real hardware.

Not yet built: any actual settings-menu UI to call `app_reader_set_font()`
(separate from the reader menu above - `settings.c` already exists and is
reachable from the app menu, not the in-book one). How the reader hooks
into `power.c`'s idle-timeout/System-OFF path (which today unmounts the
SD card unconditionally with no "notify the active screen first" hook)
is also still open.
