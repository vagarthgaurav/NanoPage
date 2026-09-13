#!/usr/bin/env python3
"""
epub2npb.py - convert an EPUB into NanoPage's on-device .npb book format.

Usage:
    python3 epub2npb.py book.epub [-o book.npb] [--title "..."] [--author "..."]

See docs/book-format.md for the format spec this implements against, and
Firmware/src/book/npb_format.h for the canonical struct definitions
npb_struct.py mirrors.
"""

import argparse
import sys

import epub_reader
import npb_struct
from transliterate import transliterate
from wrap import wrap_paragraph

# Panel is 296px wide (GDEY029T94, see .claude/CLAUDE.md); cols = floor
# division, matching docs/book-format.md's table exactly.
_PANEL_WIDTH_PX = 296

# (npb_font_id, name, glyph_width_px, glyph_height_px)
_VARIANTS = [
    (npb_struct.FONT_TAMZEN_SMALL, "TAMZEN_SMALL", 6, 16),
    (npb_struct.FONT_TAMZEN_MEDIUM, "TAMZEN_MEDIUM", 8, 16),
    (npb_struct.FONT_TAMZEN_LARGE, "TAMZEN_LARGE", 10, 24),
]


def _build_blob(paragraphs: list[str]) -> tuple[bytes, list[tuple[int, int]]]:
    """Concatenate paragraphs with no delimiter (per the format's design -
    row (offset, length) pairs fully describe content, so no separator is
    needed). Returns (blob_bytes, [(start, length), ...] per paragraph)."""
    parts = []
    ranges = []
    offset = 0
    for text in paragraphs:
        length = len(text)
        ranges.append((offset, length))
        parts.append(text)
        offset += length
    blob = "".join(parts).encode("ascii")
    assert len(blob) == offset
    return blob, ranges


def _build_rows(
    paragraphs: list[str], ranges: list[tuple[int, int]], cols: int
) -> list[tuple[int, int, int]]:
    rows: list[tuple[int, int, int]] = []
    last_index = len(paragraphs) - 1
    for i, text in enumerate(paragraphs):
        para_start, _para_len = ranges[i]
        rows.extend(wrap_paragraph(text, para_start, cols))
        if i != last_index:
            next_start = ranges[i + 1][0]
            rows.append((next_start, 0, 0))  # blank paragraph-spacer row
    return rows


def convert(epub_path: str, title_override: str | None, author_override: str | None) -> tuple[bytes, dict]:
    book = epub_reader.load(epub_path)
    title, author = epub_reader.get_metadata(book)
    title = title_override or title
    author = author_override or author

    raw_paragraphs, chapters = epub_reader.extract(book)
    if not raw_paragraphs:
        raise ValueError("no text extracted from EPUB - unexpected structure?")

    paragraphs = []
    total_substitutions = 0
    for text in raw_paragraphs:
        ascii_text, subs = transliterate(text)
        paragraphs.append(ascii_text)
        total_substitutions += subs

    blob, ranges = _build_blob(paragraphs)

    # Layout: header, variant table, one row table per variant (in variant
    # order), chapter table, text blob - each *_offset field below is an
    # absolute byte offset from file start.
    variant_table_offset = npb_struct.HEADER_SIZE
    variant_table_size = len(_VARIANTS) * npb_struct.VARIANT_DESC_SIZE

    variant_rows = []  # list of (font_id, glyph_w, glyph_h, cols, rows)
    for font_id, _name, glyph_w, glyph_h in _VARIANTS:
        cols = _PANEL_WIDTH_PX // glyph_w
        rows = _build_rows(paragraphs, ranges, cols)
        variant_rows.append((font_id, glyph_w, glyph_h, cols, rows))

    running_offset = variant_table_offset + variant_table_size
    row_table_offsets = []
    for _font_id, _glyph_w, _glyph_h, _cols, rows in variant_rows:
        row_table_offsets.append(running_offset)
        running_offset += len(rows) * npb_struct.ROW_SIZE

    chapter_table_offset = running_offset
    chapter_table_size = len(chapters) * npb_struct.CHAPTER_SIZE
    text_blob_offset = chapter_table_offset + chapter_table_size
    text_blob_size = len(blob)

    out = bytearray()
    out += npb_struct.pack_header(
        title=title,
        author=author,
        variant_count=len(_VARIANTS),
        variant_table_offset=variant_table_offset,
        chapter_count=len(chapters),
        chapter_table_offset=chapter_table_offset,
        text_blob_offset=text_blob_offset,
        text_blob_size=text_blob_size,
    )
    assert len(out) == npb_struct.HEADER_SIZE

    for (font_id, glyph_w, glyph_h, cols, rows), row_table_offset in zip(variant_rows, row_table_offsets):
        out += npb_struct.pack_variant_desc(
            font_id=font_id,
            glyph_width_px=glyph_w,
            glyph_height_px=glyph_h,
            cols=cols,
            row_count=len(rows),
            row_table_offset=row_table_offset,
        )
    assert len(out) == variant_table_offset + variant_table_size

    for (_font_id, _glyph_w, _glyph_h, _cols, rows), row_table_offset in zip(variant_rows, row_table_offsets):
        assert len(out) == row_table_offset
        for offset, length, flags in rows:
            out += npb_struct.pack_row(offset, length, flags)

    assert len(out) == chapter_table_offset
    for chapter in chapters:
        chapter_offset = ranges[chapter.paragraph_index][0]
        out += npb_struct.pack_chapter(chapter.title, chapter_offset)
    assert len(out) == text_blob_offset

    out += blob
    assert len(out) == text_blob_offset + text_blob_size

    stats = {
        "title": title,
        "author": author,
        "paragraphs": len(paragraphs),
        "chapters": len(chapters),
        "blob_size": text_blob_size,
        "substitutions": total_substitutions,
        "variants": [
            (_VARIANTS[i][1], len(variant_rows[i][4])) for i in range(len(_VARIANTS))
        ],
        "file_size": len(out),
    }
    return bytes(out), stats


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert an EPUB into NanoPage's .npb book format")
    parser.add_argument("epub_path", help="path to the source .epub file")
    parser.add_argument("-o", "--output", help="output .npb path (default: same name, .npb extension)")
    parser.add_argument("--title", help="override the book title read from EPUB metadata")
    parser.add_argument("--author", help="override the author read from EPUB metadata")
    args = parser.parse_args()

    output_path = args.output or (args.epub_path.rsplit(".", 1)[0] + ".npb")

    try:
        data, stats = convert(args.epub_path, args.title, args.author)
    except Exception as exc:  # noqa: BLE001 - top-level CLI error reporting
        print(f"error: {exc}", file=sys.stderr)
        return 1

    with open(output_path, "wb") as f:
        f.write(data)

    print(f"{stats['title']} by {stats['author']}")
    print(f"  paragraphs:    {stats['paragraphs']}")
    print(f"  chapters:      {stats['chapters']}")
    print(f"  text blob:     {stats['blob_size']} bytes")
    print(f"  substitutions: {stats['substitutions']} (non-ASCII chars replaced with '?')")
    for name, row_count in stats["variants"]:
        print(f"  {name}: {row_count} rows")
    print(f"  wrote {output_path} ({stats['file_size']} bytes)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
