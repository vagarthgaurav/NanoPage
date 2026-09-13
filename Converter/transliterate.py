"""
Unicode -> 7-bit ASCII (32-126) transliteration for .npb text blobs.

text.c's CFB fonts (font_tamzen_6x16 included) only have glyphs for the
printable ASCII range, so anything outside it must be converted or dropped
before it reaches the blob. See docs/book-format.md's "Character set"
section.
"""

import unicodedata

# Common punctuation/typography that EPUB markup uses constantly and that
# NFKD decomposition alone won't turn into ASCII (these aren't accented
# letters, they're distinct symbols with an obvious ASCII equivalent).
_SYMBOL_MAP = {
    "‘": "'",  # left single quote
    "’": "'",  # right single quote / apostrophe
    "‚": ",",  # single low-9 quote
    "“": '"',  # left double quote
    "”": '"',  # right double quote
    "„": '"',  # double low-9 quote
    "–": "-",  # en dash
    "—": "--",  # em dash
    "…": "...",  # ellipsis
    " ": " ",  # non-breaking space
    " ": " ",  # thin space
    "​": "",  # zero-width space
    "×": "x",  # multiplication sign
    "™": "(TM)",
    "©": "(C)",
    "®": "(R)",
}


def transliterate(text: str) -> tuple[str, int]:
    """Return (ascii_text, substitution_count). substitution_count counts
    characters that couldn't be mapped and were replaced with '?', so a
    conversion's lossiness can be reported to the user."""
    out = []
    substitutions = 0
    for ch in text:
        mapped = _SYMBOL_MAP.get(ch)
        if mapped is not None:
            out.append(mapped)
            continue

        cp = ord(ch)
        if 32 <= cp <= 126:
            out.append(ch)
            continue

        # Decompose accented Latin letters etc. (e.g. é 'e' -> 'e' +
        # combining acute accent) and drop the combining marks.
        decomposed = unicodedata.normalize("NFKD", ch)
        base = "".join(c for c in decomposed if not unicodedata.combining(c))
        if base and all(32 <= ord(c) <= 126 for c in base):
            out.append(base)
            continue

        out.append("?")
        substitutions += 1

    return "".join(out), substitutions
