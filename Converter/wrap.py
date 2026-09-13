"""
Greedy word-wrap: turns one paragraph's already-transliterated ASCII text
into a list of (offset, length, flags) rows over the shared text blob.

offset is an ABSOLUTE byte offset into the blob (para_start + position
within the paragraph's own text), matching struct npb_row's offset field -
callers don't need to translate afterwards. flags marks the first row of
the paragraph with NPB_ROW_FLAG_PARAGRAPH_START (see npb_struct.py) so the
on-device reader can add paragraph spacing/indent without needing a
separate lookup.

Word boundaries carry their own absolute offsets (found once via regex
over the paragraph text) rather than being recomputed from row lengths, so
wrapping never has to reverse-engineer where a word "really" started after
whitespace collapsing.
"""

import re

from npb_struct import ROW_FLAG_PARAGRAPH_START

_WORD_RE = re.compile(r"\S+")


def wrap_paragraph(text: str, para_start: int, cols: int) -> list[tuple[int, int, int]]:
    words = [(para_start + m.start(), m.group()) for m in _WORD_RE.finditer(text)]
    if not words:
        return []

    rows: list[tuple[int, int, int]] = []
    first_row_of_para = True

    def emit(offset: int, length: int) -> None:
        nonlocal first_row_of_para
        flags = ROW_FLAG_PARAGRAPH_START if first_row_of_para else 0
        rows.append((offset, length, flags))
        first_row_of_para = False

    line_start = None
    line_len = 0

    for word_offset, word in words:
        wlen = len(word)

        if wlen > cols:
            # Unbreakable token longer than a full row (e.g. a URL) - flush
            # whatever's pending, then hard-break it into cols-size chunks.
            if line_start is not None:
                emit(line_start, line_len)
                line_start, line_len = None, 0

            offset = word_offset
            remaining = wlen
            while remaining > cols:
                emit(offset, cols)
                offset += cols
                remaining -= cols
            line_start, line_len = offset, remaining
            continue

        if line_start is None:
            line_start, line_len = word_offset, wlen
            continue

        needed = line_len + 1 + wlen  # +1 for the space joining the words
        if needed > cols:
            emit(line_start, line_len)
            line_start, line_len = word_offset, wlen
        else:
            line_len = needed

    if line_start is not None:
        emit(line_start, line_len)

    return rows
