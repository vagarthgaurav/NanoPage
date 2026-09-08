#include "FooterBar.h"

#include <cstdio>

#include "Display.h"

namespace
{
    constexpr int16_t MARGIN_LEFT = 4;  // panel's left edge to the debug label
    constexpr int16_t MARGIN_RIGHT = 4; // panel's right edge to the voltage/current cluster
    constexpr int16_t GAP = 3;          // between the two clusters

    int16_t centeredBaseline(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t availableHeight)
    {
        int8_t ascent = fonts.getFontAscent();
        int8_t descent = fonts.getFontDescent(); // <= 0
        int16_t fontHeight = ascent - descent;
        return (availableHeight - fontHeight) / 2 + ascent;
    }

    int16_t footerBaseline(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t barY)
    {
        fonts.setFont(u8g2_font_6x10_tr);
        return barY + 1 + centeredBaseline(fonts, FooterBar::HEIGHT - 1) + 2;
    }

    // Widest the voltage/current cluster can plausibly be - covers a full
    // negative 4-digit mA reading, e.g. "9.99V -9999mA".
    int16_t maxStatsWidth(U8G2_FOR_ADAFRUIT_GFX &fonts)
    {
        fonts.setFont(u8g2_font_6x10_tr);
        return fonts.getUTF8Width("9.99V -9999mA");
    }
} // namespace

// The panel rounds partial windows outward to byte boundaries (see FooterBar.h's
// note on HEIGHT/y-offset alignment), which can silently pull the rule's row into
// a cluster's cleared area even though {stats,heap}Rect() ask for HEIGHT-1 -
// drawFastHLine() clips to whatever window ends up active, so redrawing it here is
// harmless even on the calls where the rounding doesn't happen to reach that row.
static void redrawRule(Display &display)
{
    auto &gfx = display.panel();
    gfx.drawFastHLine(0, gfx.height() - FooterBar::HEIGHT, gfx.width(), GxEPD_BLACK);
}

void FooterBar::drawStats(Display &display, float currentMilliAmps, float voltageMilliVolts)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();
    int16_t panelW = gfx.width();
    int16_t barY = gfx.height() - HEIGHT;
    int16_t baseline = footerBaseline(fonts, barY);

    // Positive current = charging (MAX17055 convention), so the sign doubles as a charge/discharge tell.
    char statsLabel[24];
    snprintf(statsLabel, sizeof(statsLabel), "%.2fV %+.0fmA", voltageMilliVolts / 1000.0f, currentMilliAmps);
    fonts.setFont(u8g2_font_6x10_tr);
    int16_t statsW = fonts.getUTF8Width(statsLabel);
    int16_t statsX = panelW - MARGIN_RIGHT - statsW;
    fonts.setCursor(statsX, baseline);
    fonts.print(statsLabel);
    redrawRule(display);
}

void FooterBar::statsRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h)
{
    auto &gfx = display.panel();
    int16_t panelW = gfx.width();
    w = MARGIN_RIGHT + maxStatsWidth(display.fonts());
    x = panelW - w;
    y = gfx.height() - HEIGHT;
    h = HEIGHT - 1; // see the rounding note above redrawRule()
}

void FooterBar::drawHeap(Display &display, uint32_t freeHeapBytes)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();
    int16_t barY = gfx.height() - HEIGHT;
    int16_t baseline = footerBaseline(fonts, barY);

    char heapLabel[24];
    snprintf(heapLabel, sizeof(heapLabel), "%luKB free", (unsigned long)(freeHeapBytes / 1024));
    fonts.setFont(u8g2_font_6x10_tr);
    // Assume the stats cluster is at its widest possible extent, same as draw() does
    // implicitly by being called after it - keeps this independent of the actual
    // current stats width, which the caller may not be redrawing right now.
    int16_t maxWidth = gfx.width() - MARGIN_RIGHT - maxStatsWidth(fonts) - GAP - MARGIN_LEFT;
    if (fonts.getUTF8Width(heapLabel) <= maxWidth)
    {
        fonts.setCursor(MARGIN_LEFT, baseline);
        fonts.print(heapLabel);
    } // else not enough room - skip rather than overlap the stats cluster
    redrawRule(display);
}

void FooterBar::heapRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h)
{
    auto &gfx = display.panel();
    x = MARGIN_LEFT;
    w = gfx.width() - MARGIN_RIGHT - maxStatsWidth(display.fonts()) - GAP - MARGIN_LEFT;
    y = gfx.height() - HEIGHT;
    h = HEIGHT - 1; // see the rounding note above redrawRule()
}

void FooterBar::draw(Display &display, float currentMilliAmps, float voltageMilliVolts, uint32_t freeHeapBytes)
{
    // Each of these redraws the rule too (see redrawRule()) - redundant here but harmless.
    drawStats(display, currentMilliAmps, voltageMilliVolts);
    drawHeap(display, freeHeapBytes);
}
