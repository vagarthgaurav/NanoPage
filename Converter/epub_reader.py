"""
EPUB -> plain paragraphs + chapter markers. Everything EPUB-specific
(zip container, OPF manifest/spine, XHTML, nav/NCX toc) lives here; the
rest of the converter only ever sees a flat list of paragraph strings.

Known limitation (v1): a table-of-contents entry that points mid-document
(an href with a #fragment anchor) is matched to the start of that whole
document, not the anchor itself - splitting a document at an arbitrary
internal anchor would need tracking byte offsets through BeautifulSoup's
parse, which isn't done yet. Most EPUBs put one chapter per XHTML file, so
this covers the common case; a book that packs multiple chapters into one
file will have its later chapter titles recorded but pointing at the same
paragraph as the first.
"""

import re
import warnings

import ebooklib
from bs4 import BeautifulSoup, XMLParsedAsHTMLWarning
from ebooklib import epub

# EPUB XHTML documents carry an XML declaration; bs4 flags that as a hint
# to use its XML parser, but we deliberately want HTML-mode parsing (more
# forgiving of the malformed markup real-world EPUBs contain).
warnings.filterwarnings("ignore", category=XMLParsedAsHTMLWarning)

_BLOCK_TAGS = ("p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote")
_WHITESPACE_RE = re.compile(r"\s+")

try:
    import lxml  # noqa: F401

    _HTML_PARSER = "lxml"
except ImportError:
    _HTML_PARSER = "html.parser"


class Chapter:
    def __init__(self, title: str, paragraph_index: int):
        self.title = title
        self.paragraph_index = paragraph_index  # index into the flat paragraph list


def load(path: str) -> epub.EpubBook:
    return epub.read_epub(path, options={"ignore_ncx": False})


def get_metadata(book: epub.EpubBook) -> tuple[str, str]:
    titles = book.get_metadata("DC", "title")
    creators = book.get_metadata("DC", "creator")
    title = titles[0][0] if titles else "Unknown Title"
    author = creators[0][0] if creators else "Unknown Author"
    return title, author


def _reading_order(book: epub.EpubBook) -> list:
    items = []
    for idref, _linear in book.spine:
        item = book.get_item_with_id(idref)
        if item is not None and item.get_type() == ebooklib.ITEM_DOCUMENT:
            items.append(item)
    return items


def _flatten_toc(toc_items) -> list[tuple[str, str]]:
    """Returns a flat list of (title, href) in document order, href with
    any #fragment stripped."""
    flat = []
    for item in toc_items:
        if isinstance(item, tuple):
            section, children = item
            title = getattr(section, "title", None)
            href = getattr(section, "href", None)
            if title:
                flat.append((title, (href or "").split("#")[0]))
            flat.extend(_flatten_toc(children))
        elif isinstance(item, epub.Link):
            flat.append((item.title, item.href.split("#")[0]))
        elif isinstance(item, epub.Section):
            flat.append((item.title, (getattr(item, "href", "") or "").split("#")[0]))
    return flat


def _extract_paragraphs(item) -> list[str]:
    soup = BeautifulSoup(item.get_content(), _HTML_PARSER)
    for tag in soup.find_all(("script", "style", "nav")):
        tag.decompose()

    paragraphs = []
    for tag in soup.find_all(_BLOCK_TAGS):
        text = tag.get_text(separator=" ", strip=True)
        text = _WHITESPACE_RE.sub(" ", text).strip()
        if text:
            paragraphs.append(text)
    return paragraphs


def extract(book: epub.EpubBook) -> tuple[list[str], list[Chapter]]:
    """Returns (paragraphs, chapters). paragraphs is the full flat list in
    reading order; chapters points into it by paragraph index."""
    toc_by_href = {}
    for title, href in _flatten_toc(book.toc):
        toc_by_href.setdefault(href, title)  # first toc entry per doc wins

    paragraphs: list[str] = []
    chapters: list[Chapter] = []

    for item in _reading_order(book):
        name = item.get_name()
        matched_title = toc_by_href.get(name)
        doc_paragraphs = _extract_paragraphs(item)
        if not doc_paragraphs:
            continue
        if matched_title is not None:
            chapters.append(Chapter(matched_title, len(paragraphs)))
        paragraphs.extend(doc_paragraphs)

    return paragraphs, chapters
