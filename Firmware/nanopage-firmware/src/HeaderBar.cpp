#include "HeaderBar.h"

#include <cstdio>
#include <cstring>

#include "Display.h"

namespace
{
    constexpr uint16_t CHARGING_GLYPH = 0xe216;
    constexpr uint16_t BATTERY_GLYPH_MIN = 0xe24c;
    constexpr uint16_t BATTERY_GLYPH_MAX = 0xe254; // 9 fill levels, 0..8
    constexpr uint8_t BATTERY_GLYPH_COUNT = BATTERY_GLYPH_MAX - BATTERY_GLYPH_MIN + 1;

    constexpr int16_t MARGIN_LEFT = 4;  // panel's left edge to the label
    constexpr int16_t MARGIN_RIGHT = 4; // panel's right edge to the battery icon
    constexpr int16_t GAP = 3;          // between glyphs along the bar

    int16_t centeredBaseline(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t availableHeight)
    {
        int8_t ascent = fonts.getFontAscent();
        int8_t descent = fonts.getFontDescent(); // <= 0
        int16_t fontHeight = ascent - descent;
        return (availableHeight - fontHeight) / 2 + ascent;
    }

    // Draws the battery icon/%/charging-icon cluster, right-aligned to panelW,
    // and returns the leftmost x it used (i.e. where the label may run up to).
    int16_t drawBatteryClusterImpl(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t panelW, int16_t contentH,
                                    uint8_t batteryPercent, bool charging)
    {
        fonts.setFont(u8g2_font_siji_t_6x10);
        int16_t iconBaseline = centeredBaseline(fonts, contentH);

        uint8_t level = (uint8_t)((batteryPercent * (BATTERY_GLYPH_COUNT - 1) + 50) / 100);
        uint16_t batteryGlyph = BATTERY_GLYPH_MIN + level;
        int16_t batteryW = u8g2_GetGlyphWidth(&fonts.u8g2, batteryGlyph);
        int16_t batteryX = panelW - MARGIN_RIGHT - batteryW;
        fonts.drawGlyph(batteryX, iconBaseline, batteryGlyph);

        fonts.setFont(u8g2_font_6x10_tr);
        int16_t textBaseline = centeredBaseline(fonts, contentH);
        char pctLabel[5];
        snprintf(pctLabel, sizeof(pctLabel), "%u%%", batteryPercent);
        int16_t textW = fonts.getUTF8Width(pctLabel);
        int16_t textX = batteryX - GAP - textW;
        fonts.setCursor(textX, textBaseline);
        fonts.print(pctLabel);

        int16_t reservedX = textX; // leftmost x used by the battery/% cluster so far
        if (charging)
        {
            fonts.setFont(u8g2_font_siji_t_6x10);
            int16_t chargingW = u8g2_GetGlyphWidth(&fonts.u8g2, CHARGING_GLYPH);
            int16_t chargingX = textX - GAP - chargingW;
            fonts.drawGlyph(chargingX, iconBaseline, CHARGING_GLYPH);
            reservedX = chargingX;
        }
        return reservedX;
    }

    // Extra slack on top of the computed worst-case width - the battery/charging
    // icons are custom glyph-font bitmaps that can render a few pixels past the
    // "advance width" u8g2_GetGlyphWidth() reports, which was leaving slivers of
    // the old icon uncleared at the left edge of the window.
    constexpr int16_t SAFETY_MARGIN = 6;

    // Widest the cluster can possibly be (3-digit percent, charging shown) -
    // used to size a partial-refresh window that's safely big enough to cover
    // the cluster in any percent/charging combination, old or new.
    int16_t maxBatteryClusterWidth(U8G2_FOR_ADAFRUIT_GFX &fonts)
    {
        fonts.setFont(u8g2_font_siji_t_6x10);
        int16_t iconW = u8g2_GetGlyphWidth(&fonts.u8g2, BATTERY_GLYPH_MAX);
        int16_t chargingW = u8g2_GetGlyphWidth(&fonts.u8g2, CHARGING_GLYPH);
        fonts.setFont(u8g2_font_6x10_tr);
        int16_t textW = fonts.getUTF8Width("100%");
        return iconW + GAP + textW + GAP + chargingW + SAFETY_MARGIN;
    }
} // namespace

void HeaderBar::drawBatteryCluster(Display &display, uint8_t batteryPercent, bool charging)
{
    if (batteryPercent > 100)
        batteryPercent = 100;
    auto &gfx = display.panel();
    auto &fonts = display.fonts();
    drawBatteryClusterImpl(fonts, gfx.width(), HEIGHT - 1, batteryPercent, charging);
    // The panel rounds partial windows outward to byte boundaries (see FooterBar.h),
    // which can silently pull the rule's row into this window's cleared area even
    // though batteryClusterRect() asks for HEIGHT-1 - redraw it so it isn't erased.
    // drawFastHLine() clips to whatever window ends up active, so this is harmless
    // even when the rounding doesn't happen to reach that row.
    gfx.drawFastHLine(0, HEIGHT - 1, gfx.width(), GxEPD_BLACK);
}

void HeaderBar::batteryClusterRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h)
{
    auto &gfx = display.panel();
    int16_t panelW = gfx.width();
    w = MARGIN_RIGHT + maxBatteryClusterWidth(display.fonts());
    x = panelW - w;
    y = 0;
    h = HEIGHT - 1; // see the rounding note in drawBatteryCluster()
}

void HeaderBar::draw(Display &display, uint8_t batteryPercent, bool charging, const char *label)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    if (batteryPercent > 100)
        batteryPercent = 100;

    int16_t panelW = gfx.width();
    int16_t contentH = HEIGHT - 1; // excludes the bottom rule

    int16_t reservedX = drawBatteryClusterImpl(fonts, panelW, contentH, batteryPercent, charging);
    int16_t textBaseline = centeredBaseline(fonts, contentH);

    if (label && *label)
    {
        fonts.setFont(u8g2_font_6x10_tr);
        int16_t maxWidth = reservedX - GAP - MARGIN_LEFT;
        char clipped[64];
        size_t len = strlen(label);
        if (len >= sizeof(clipped))
            len = sizeof(clipped) - 1;
        memcpy(clipped, label, len);
        clipped[len] = '\0';
        while (len > 0 && fonts.getUTF8Width(clipped) > maxWidth)
            clipped[--len] = '\0';

        fonts.setCursor(MARGIN_LEFT, textBaseline);
        fonts.print(clipped);
    }

    gfx.drawFastHLine(0, HEIGHT - 1, panelW, GxEPD_BLACK);
}
