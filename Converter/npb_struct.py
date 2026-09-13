"""
Binary layout for .npb files - mirrors Firmware/src/book/npb_format.h exactly.

There is no shared codegen between the C headers and this file: if
npb_format.h changes, these struct format strings must be updated by hand
to match. Each pack_* function's docstring cites the C struct it mirrors
so a future diff is easy to spot.
"""

import struct

MAGIC = b"NPBK"
VERSION = 1

# enum npb_font_id (npb_format.h)
FONT_TAMZEN_SMALL = 0
FONT_TAMZEN_MEDIUM = 1
FONT_TAMZEN_LARGE = 2

# struct npb_row.flags bits
ROW_FLAG_PARAGRAPH_START = 1 << 0

HEADER_SIZE = 256
VARIANT_DESC_SIZE = 24
ROW_SIZE = 8
CHAPTER_SIZE = 72

TITLE_LEN = 64
AUTHOR_LEN = 64
CHAPTER_TITLE_LEN = 64

# Little-endian, no padding (matches __attribute__((packed)) on the nRF52840,
# which is itself little-endian, so the on-disk bytes need no translation).
_HEADER_FMT = "<4sHH64s64sIIIIII96s"
_VARIANT_FMT = "<BBBBHHIIQ"
_ROW_FMT = "<IBBH"
_CHAPTER_FMT = "<64sII"

assert struct.calcsize(_HEADER_FMT) == HEADER_SIZE
assert struct.calcsize(_VARIANT_FMT) == VARIANT_DESC_SIZE
assert struct.calcsize(_ROW_FMT) == ROW_SIZE
assert struct.calcsize(_CHAPTER_FMT) == CHAPTER_SIZE


def _fixed_str(s: str, size: int) -> bytes:
    """UTF-8 encode, truncating whole characters (never mid-codepoint) to
    fit `size` bytes, then NUL-pad to size."""
    b = s.encode("utf-8")
    while len(b) > size:
        s = s[:-1]
        b = s.encode("utf-8")
    return b + b"\x00" * (size - len(b))


def pack_header(
    title: str,
    author: str,
    variant_count: int,
    variant_table_offset: int,
    chapter_count: int,
    chapter_table_offset: int,
    text_blob_offset: int,
    text_blob_size: int,
) -> bytes:
    """struct npb_header"""
    return struct.pack(
        _HEADER_FMT,
        MAGIC,
        VERSION,
        0,  # reserved0
        _fixed_str(title, TITLE_LEN),
        _fixed_str(author, AUTHOR_LEN),
        variant_count,
        variant_table_offset,
        chapter_count,
        chapter_table_offset,
        text_blob_offset,
        text_blob_size,
        b"\x00" * 96,  # reserved1
    )


def pack_variant_desc(
    font_id: int,
    glyph_width_px: int,
    glyph_height_px: int,
    cols: int,
    row_count: int,
    row_table_offset: int,
) -> bytes:
    """struct npb_variant_desc"""
    return struct.pack(
        _VARIANT_FMT,
        font_id,
        glyph_width_px,
        glyph_height_px,
        0,  # reserved0
        cols,
        0,  # reserved1
        row_count,
        row_table_offset,
        0,  # reserved2
    )


def pack_row(offset: int, length: int, flags: int) -> bytes:
    """struct npb_row"""
    if length > 255:
        raise ValueError(f"row length {length} exceeds uint8_t (a wrap bug upstream)")
    return struct.pack(_ROW_FMT, offset, length, flags, 0)


def pack_chapter(title: str, text_offset: int) -> bytes:
    """struct npb_chapter"""
    return struct.pack(_CHAPTER_FMT, _fixed_str(title, CHAPTER_TITLE_LEN), text_offset, 0)
