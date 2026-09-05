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
} // namespace

void FooterBar::draw(Display &display, float currentMilliAmps, float voltageMilliVolts, uint32_t freeHeapBytes)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    int16_t panelW = gfx.width();
    int16_t barY = gfx.height() - HEIGHT;
    int16_t contentH = HEIGHT - 1; // excludes the top rule

    gfx.drawFastHLine(0, barY, panelW, GxEPD_BLACK);

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t baseline = barY + 1 + centeredBaseline(fonts, contentH) + 2;

    // Positive current = charging (MAX17055 convention), so the sign doubles as a charge/discharge tell.
    char statsLabel[24];
    snprintf(statsLabel, sizeof(statsLabel), "%.2fV %+.0fmA", voltageMilliVolts / 1000.0f, currentMilliAmps);
    int16_t statsW = fonts.getUTF8Width(statsLabel);
    int16_t statsX = panelW - MARGIN_RIGHT - statsW;
    fonts.setCursor(statsX, baseline);
    fonts.print(statsLabel);

    char heapLabel[24];
    snprintf(heapLabel, sizeof(heapLabel), "%luKB free", (unsigned long)(freeHeapBytes / 1024));
    int16_t maxWidth = statsX - GAP - MARGIN_LEFT;
    if (fonts.getUTF8Width(heapLabel) > maxWidth)
        return; // not enough room - skip rather than overlap the stats cluster

    fonts.setCursor(MARGIN_LEFT, baseline);
    fonts.print(heapLabel);
}
