#include "components/FileBrowser.h"

#include <cstring>

#include "Display.h"

namespace
{
    // siji icon codepoints.
    constexpr uint16_t ICON_CURSOR = 0xe058;
    constexpr uint16_t ICON_FOLDER = 0xe1e0;
    constexpr uint16_t ICON_FILE = 0xe1cf;

    // HEADER_HEIGHT/FOOTER_HEIGHT/ROW_HEIGHT must all be multiples of 8 - see
    // Display::refreshPartial().
    constexpr int16_t HEADER_HEIGHT = Header::HEIGHT;
    constexpr int16_t FOOTER_HEIGHT = Footer::HEIGHT;
    constexpr int16_t CONTENT_TOP = HEADER_HEIGHT;
    constexpr int16_t ROW_HEIGHT = 16;
    constexpr int16_t CURSOR_X = 2;
    constexpr int16_t ICON_X = 16;
    constexpr int16_t TEXT_X = 30;
} // namespace

void FileBrowser::begin(Display &display)
{
    reload();
    display.refreshFull([&]()
                         { drawAll(display); });
}

void FileBrowser::reload()
{
    entryCount_ = 0;
    selected_ = 0;
    scrollTop_ = 0;

    if (strcmp(path_, "/") != 0)
    {
        strcpy(entries_[0].name, "..");
        entries_[0].isDir = true;
        entryCount_ = 1;
    }

    uint8_t listed = 0;
    mounted_ = SdCardStorage::listDir(path_, entries_ + entryCount_, SdCardStorage::MAX_ENTRIES - entryCount_, listed);
    entryCount_ += listed;
}

void FileBrowser::prev(Display &display)
{
    if (selected_ == 0)
        return;
    moveSelection(display, selected_ - 1);
}

void FileBrowser::next(Display &display)
{
    if (entryCount_ == 0 || selected_ + 1 >= entryCount_)
        return;
    moveSelection(display, selected_ + 1);
}

void FileBrowser::moveSelection(Display &display, uint8_t newSelected)
{
    auto &gfx = display.panel();
    int16_t contentHeight = gfx.height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    uint8_t rowsVisible = (uint8_t)(contentHeight / ROW_HEIGHT);

    uint8_t oldSelected = selected_;
    selected_ = newSelected;

    uint8_t previousScrollTop = scrollTop_;
    if (selected_ < scrollTop_)
        scrollTop_ = selected_;
    else if (selected_ >= scrollTop_ + rowsVisible)
        scrollTop_ = selected_ - rowsVisible + 1;

    if (scrollTop_ != previousScrollTop)
    {
        // Scrolled - the whole visible page changed, not just one row. Window
        // extends through the footer too (not just contentHeight) so drawAll()'s
        // footer_.draw() actually lands within the refreshed region.
        display.refresh(0, CONTENT_TOP, gfx.width(), contentHeight + FOOTER_HEIGHT,
                         [&]()
                         { drawAll(display); },
                         [&]()
                         { drawAll(display); });
        return;
    }

    // Single-step move, same page - old and new selected rows are adjacent.
    uint8_t rowA = oldSelected - scrollTop_;
    uint8_t rowB = selected_ - scrollTop_;
    uint8_t topRow = rowA < rowB ? rowA : rowB;

    display.refresh(0, CONTENT_TOP + topRow * ROW_HEIGHT, gfx.width(), 2 * ROW_HEIGHT,
                     [&]()
                     {
                         drawRow(display, rowA);
                         drawRow(display, rowB);
                     },
                     [&]()
                     { drawAll(display); });
}

void FileBrowser::activate(Display &display)
{
    if (!mounted_ || entryCount_ == 0)
        return;

    const SdCardStorage::Entry &sel = entries_[selected_];
    if (!sel.isDir)
        return;

    if (strcmp(sel.name, "..") == 0)
    {
        char *slash = strrchr(path_, '/');
        if (slash && slash != path_)
            *slash = '\0';
        else
            strcpy(path_, "/");
    }
    else
    {
        size_t len = strlen(path_);
        size_t sep = (len > 1) ? 1 : 0;
        if (len + sep + strlen(sel.name) >= MAX_PATH_LEN)
            return; // path too deep, ignore
        if (sep)
            path_[len++] = '/';
        strcpy(path_ + len, sel.name);
    }

    reload();
    display.refreshFull([&]()
                         { drawAll(display); });
}

void FileBrowser::drawAll(Display &display)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    header_.draw(display, path_);
    footer_.draw(display);

    if (!mounted_)
    {
        fonts.setFont(u8g2_font_6x10_tr);
        fonts.setCursor(TEXT_X, CONTENT_TOP + fonts.getFontAscent() + 2);
        fonts.print("SD card not found");
        return;
    }

    int16_t contentHeight = gfx.height() - HEADER_HEIGHT - FOOTER_HEIGHT;
    uint8_t rowsVisible = (uint8_t)(contentHeight / ROW_HEIGHT);

    for (uint8_t row = 0; row < rowsVisible; row++)
        drawRow(display, row);
}

void FileBrowser::drawRow(Display &display, uint8_t row)
{
    uint8_t idx = scrollTop_ + row;
    if (idx >= entryCount_)
        return;

    auto &fonts = display.fonts();
    int16_t rowY = CONTENT_TOP + row * ROW_HEIGHT;

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t baseline = rowY + fonts.getFontAscent() + 2;

    fonts.setFont(u8g2_font_siji_t_6x10);
    if (idx == selected_)
        fonts.drawGlyph(CURSOR_X, baseline, ICON_CURSOR);
    fonts.drawGlyph(ICON_X, baseline, entries_[idx].isDir ? ICON_FOLDER : ICON_FILE);

    fonts.setFont(u8g2_font_6x10_tr);
    fonts.setCursor(TEXT_X, baseline);
    fonts.print(entries_[idx].name);
}
