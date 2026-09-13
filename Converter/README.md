# NanoPage EPUB → .npb converter

PC-side tool that turns an ordinary EPUB into the `.npb` format the
NanoPage reader firmware understands. See
[`../docs/book-format.md`](../docs/book-format.md) for the full format
spec and rationale, and
[`../Firmware/src/book/npb_format.h`](../Firmware/src/book/npb_format.h)
for the canonical on-disk struct definitions `npb_struct.py` mirrors by
hand (no shared codegen between C and Python yet — if the header
changes, update `npb_struct.py` to match).

## Setup

```sh
cd Converter
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## Usage

```sh
python3 epub2npb.py mybook.epub
# writes mybook.npb next to it

python3 epub2npb.py mybook.epub -o /path/to/output.npb --title "Override Title" --author "Override Author"
```

Prints a summary: paragraph/chapter counts, text blob size, how many
characters were transliterated to `?` (a measure of how lossy the
conversion was — worth checking on anything but a plain-English ASCII
book), and row counts for each of the three pre-wrapped font sizes.

## What it does

1. Unzips the EPUB and walks the OPF spine for reading order (via
   `ebooklib`).
2. Strips each XHTML document down to block-level text (`<p>`, headings,
   `<li>`, `<blockquote>`) via `BeautifulSoup`, dropping all markup,
   images, and styling — v1 is a 1-bit text-grid renderer, not a general
   document viewer.
3. Matches the EPUB's nav/NCX table of contents to the document that
   starts each chapter, recording chapter titles + their text-blob byte
   offset.
4. Transliterates to 7-bit ASCII (curly quotes → straight, em/en dash →
   `--`/`-`, accented Latin → unaccented, anything else → `?`).
5. Concatenates every paragraph into one shared text blob (no
   delimiters — every row's `(offset, length)` is self-describing).
6. Word-wraps that blob three times, once per Tamzen font size
   (6×16/8×16/10×24 px, i.e. 49/37/29 columns on the 296px panel), each
   producing its own row table over the *same* blob.
7. Packs header + variant tables + row tables + chapter table + blob
   into one `.npb` file, per the layout in `docs/book-format.md`.

## Known limitations (v1)

- A table-of-contents entry pointing at a `#fragment` anchor mid-document
  is matched to the start of that whole document, not the anchor itself —
  fine for the common one-chapter-per-file EPUB, imprecise for a book
  that packs multiple chapters into a single XHTML file (see
  `epub_reader.py`'s module docstring).
- Images are dropped entirely — no cover, no illustrations.
- Paragraph detection is tag-based (`<p>`/headings/`<li>`/`<blockquote>`);
  an EPUB that uses bare `<div>`s for paragraphs instead of `<p>` will
  extract as one giant paragraph per document. Not seen in the books
  tested so far; revisit if it comes up.

## Not built yet

The on-device reader (`Firmware/src/book/reader.c`) that actually opens
a `.npb` file and its `.npb.bmk` bookmark sidecar — this converter only
produces the file, nothing reads it yet.
