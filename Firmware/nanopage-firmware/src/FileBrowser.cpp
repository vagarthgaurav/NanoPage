#include "FileBrowser.h"

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>
#include <string.h>

#include "Display.h"
#include "FooterBar.h"
#include "HeaderBar.h"

namespace
{
    // siji icon codepoints - cursor, folder, file (in on-screen order).
    constexpr uint16_t ICON_CURSOR = 0xe09a;
    constexpr uint16_t ICON_FOLDER = 0xe1e0;
    constexpr uint16_t ICON_FILE = 0xe1ed;

    constexpr int16_t ROW_HEIGHT = 14;
    constexpr int16_t TOP_PADDING = 4; // gap between the header rule and the first row
    constexpr int16_t CURSOR_COL_X = 2;
    constexpr int16_t ICON_COL_X = 14;
    constexpr int16_t TEXT_COL_X = 27;

    SdFat sd;

    int16_t centeredBaseline(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t availableHeight)
    {
        int8_t ascent = fonts.getFontAscent();
        int8_t descent = fonts.getFontDescent(); // <= 0
        int16_t fontHeight = ascent - descent;
        return (availableHeight - fontHeight) / 2 + ascent;
    }
} // namespace

bool FileBrowser::begin()
{
    strcpy(path_, "/");
    listCurrentDir();
    return mounted_;
}

// Powers the card up, mounts it, reads the directory, then powers it back down.
void FileBrowser::listCurrentDir()
{
    entryCount_ = 0;
    selected_ = 0;
    scrollTop_ = 0;

    digitalWrite(PIN_SD_ON, HIGH); // Display::begin() leaves it off
    delay(10);                     // let its supply rail settle before talking SPI

    mounted_ = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
    if (mounted_)
    {
        if (strcmp(path_, "/") != 0 && entryCount_ < MAX_ENTRIES)
        {
            strcpy(entries_[entryCount_].name, "..");
            entries_[entryCount_].isDir = true;
            entryCount_++;
        }

        File dir = sd.open(path_);
        if (dir)
        {
            File entry;
            while (entryCount_ < MAX_ENTRIES && (entry = dir.openNextFile()))
            {
                entry.getName(entries_[entryCount_].name, MAX_NAME_LEN);
                entries_[entryCount_].isDir = entry.isDirectory();
                entry.close();
                entryCount_++;
            }
            dir.close();
        }
    }

    digitalWrite(PIN_SD_ON, LOW); // cut power until the next read
}

void FileBrowser::next()
{
    if (entryCount_ == 0 || selected_ + 1 >= entryCount_)
        return;
    selected_++;
}

void FileBrowser::prev()
{
    if (selected_ == 0)
        return;
    selected_--;
}

bool FileBrowser::activateSelected()
{
    if (!mounted_ || entryCount_ == 0)
        return false;

    const Entry &sel = entries_[selected_];
    if (!sel.isDir)
        return false;

    if (strcmp(sel.name, "..") == 0)
    {
        char *slash = strrchr(path_, '/');
        if (slash && slash != path_)
            *slash = '\0'; // drop the last path component
        else
            strcpy(path_, "/");
    }
    else
    {
        size_t len = strlen(path_);
        if (len > 1)
            path_[len++] = '/';
        strncpy(path_ + len, sel.name, MAX_PATH_LEN - len - 1);
        path_[MAX_PATH_LEN - 1] = '\0';
    }

    listCurrentDir();
    return true;
}

void FileBrowser::ensureVisible(uint8_t rowsVisible)
{
    if (selected_ < scrollTop_)
        scrollTop_ = selected_;
    else if (rowsVisible > 0 && selected_ >= scrollTop_ + rowsVisible)
        scrollTop_ = selected_ - rowsVisible + 1;
}

void FileBrowser::drawSelectionDelta(Display &display, uint8_t previousSelected)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    int16_t contentY = HeaderBar::HEIGHT + TOP_PADDING;
    int16_t contentH = gfx.height() - FooterBar::HEIGHT - contentY;
    uint8_t rowsVisible = (uint8_t)(contentH / ROW_HEIGHT);

    uint8_t previousScrollTop = scrollTop_;
    ensureVisible(rowsVisible); // may move scrollTop_ to keep the new selected_ in view

    if (scrollTop_ != previousScrollTop)
    {
        // The visible page shifted - every row's content changed, not just the
        // arrow - but it's still just the content area, not the whole panel.
        display.refreshPartial(0, HeaderBar::HEIGHT, gfx.width(), contentH + TOP_PADDING, [&]()
                                { draw(display); });
        return;
    }

    // previousSelected and selected_ are both still on screen, and (since prev()/next()
    // only move by one row) adjacent.
    int16_t oldRow = previousSelected - scrollTop_;
    int16_t newRow = selected_ - scrollTop_;
    int16_t topRow = oldRow < newRow ? oldRow : newRow;
    int16_t rowSpan = (oldRow > newRow ? oldRow : newRow) - topRow + 1;

    // One extra row of margin above and below, clamped to the visible page - the
    // cursor/folder-file glyphs are custom bitmap fonts that can render outside
    // their reported advance width/height, which left slivers uncleared right at
    // the edge of a tightly-sized window. Cheaper than pinning down their exact
    // pixel bounds, and redraws (not blanks) whatever falls in the margin rows.
    int16_t clearTopRow = topRow > 0 ? topRow - 1 : 0;
    int16_t clearRowSpan = (topRow + rowSpan + 1) - clearTopRow;
    if (clearTopRow + clearRowSpan > rowsVisible)
        clearRowSpan = rowsVisible - clearTopRow;

    int16_t y = contentY + clearTopRow * ROW_HEIGHT;
    int16_t h = clearRowSpan * ROW_HEIGHT;

    // Covers both the cursor arrow and the folder/file icon next to it, not just
    // the tight gap between them, for the same reason as the vertical margin above.
    // Redraws the type icon too (even though it doesn't change) so nothing between
    // the two, or in the margin rows, gets left half-cleared.
    display.refreshPartial(0, y, TEXT_COL_X, h, [&]()
                            {
        fonts.setFont(u8g2_font_siji_t_6x10);
        for (int16_t row = clearTopRow; row < clearTopRow + clearRowSpan; row++)
        {
            uint8_t idx = scrollTop_ + row;
            if (idx >= entryCount_)
                break;
            int16_t baseline = contentY + row * ROW_HEIGHT + centeredBaseline(fonts, ROW_HEIGHT);
            if (idx == selected_)
                fonts.drawGlyph(CURSOR_COL_X, baseline, ICON_CURSOR);
            fonts.drawGlyph(ICON_COL_X, baseline, entries_[idx].isDir ? ICON_FOLDER : ICON_FILE);
        } });
}

void FileBrowser::draw(Display &display)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    int16_t contentY = HeaderBar::HEIGHT + TOP_PADDING;
    int16_t contentH = gfx.height() - FooterBar::HEIGHT - contentY;
    uint8_t rowsVisible = (uint8_t)(contentH / ROW_HEIGHT);

    ensureVisible(rowsVisible);

    if (!mounted_)
    {
        fonts.setFont(u8g2_font_6x10_tr);
        fonts.setCursor(TEXT_COL_X, contentY + centeredBaseline(fonts, ROW_HEIGHT));
        fonts.print("SD card not found");
        return;
    }

    for (uint8_t row = 0; row < rowsVisible; row++)
    {
        uint8_t idx = scrollTop_ + row;
        if (idx >= entryCount_)
            break;

        int16_t rowY = contentY + row * ROW_HEIGHT;
        int16_t baseline = rowY + centeredBaseline(fonts, ROW_HEIGHT);

        if (idx == selected_)
        {
            fonts.setFont(u8g2_font_siji_t_6x10);
            fonts.drawGlyph(CURSOR_COL_X, baseline, ICON_CURSOR);
        }

        fonts.setFont(u8g2_font_siji_t_6x10);
        fonts.drawGlyph(ICON_COL_X, baseline, entries_[idx].isDir ? ICON_FOLDER : ICON_FILE);

        fonts.setFont(u8g2_font_6x10_tr);
        fonts.setCursor(TEXT_COL_X, baseline);
        fonts.print(entries_[idx].name);
    }
}
