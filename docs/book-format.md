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
and `font_siji_icons`) is separate font/firmware work, not part of this
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
`<book>.npb.bmk`, next to it on the SD card and owned entirely by the
device. This means re-converting a book (a fixed typo, an added font
size) never risks wiping a reader's saved places, and the read-only
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
the auto-resume position and chapter jumps do. Bookmark slot management
policy (fixed capacity vs. overwrite-oldest vs. letting the user pick
which to delete, etc.) is reader-UI behavior, not format — deferred along
with the rest of the reader implementation.

## Size sanity check

A ~90,000-word novel (~500KB of text) wraps to roughly 10,000 rows at 49
columns (smallest font). Per variant: `10,000 × 8 bytes ≈ 80KB` row
table. Three variants: ~240KB of row tables + ~500KB shared blob ≈ 750KB
per book, plus a negligible chapter table. Trivial for an SD card — file
size was never a constraint here, which is what lets the
row-table-per-character-span design stay simple instead of needing any
compression.

## What's implemented so far

Only the two header files with struct definitions:

- `Firmware/src/book/npb_format.h` — `npb_header`, `npb_variant_desc`,
  `npb_row`, `npb_chapter`, `enum npb_font_id`.
- `Firmware/src/book/npb_bookmark_format.h` — `npb_bookmark_file`,
  `npb_bookmark`.

Nothing reads or writes these structs yet. Two independent next steps,
either of which can come first:

1. **The PC-side EPUB → `.npb` converter.** Unzip the EPUB, walk the OPF
   manifest/spine for reading order, strip each XHTML chapter to plain
   paragraphs, transliterate to ASCII, greedily word-wrap per font
   variant's column count, and emit the file per the layout above.
2. **The on-device reader screen** (`Firmware/src/book/reader.c`, a
   structural sibling of `Firmware/src/ui/browser.c`): opens a `.npb`
   file the browser hands off to it, owns its own button callback and
   refresh-policy counter (same pattern as `browser.c`), and reads/writes
   the `.npb.bmk` sidecar. Needs its own plan — button mapping, whether
   header/footer chrome stays visible while reading, and how it hooks
   into `power.c`'s idle-timeout/System-OFF path (which today unmounts
   the SD card unconditionally with no "notify the active screen first"
   hook — see the exploration notes from the 2026-09-13 planning session
   for the exact gap) are all still open.
