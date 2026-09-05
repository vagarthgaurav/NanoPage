#include "HeaderBar.h"

#include <cstdio>

#include "Display.h"

namespace
{
    constexpr uint16_t CHARGING_GLYPH = 0xe216;
    constexpr uint16_t BATTERY_GLYPH_MIN = 0xe24c;
    constexpr uint16_t BATTERY_GLYPH_MAX = 0xe254; // 9 fill levels, 0..8
    constexpr uint8_t BATTERY_GLYPH_COUNT = BATTERY_GLYPH_MAX - BATTERY_GLYPH_MIN + 1;

    constexpr int16_t MARGIN_RIGHT = 4; // panel's right edge to the battery icon
    constexpr int16_t GAP = 3;          // between glyphs along the bar

    // Baseline that centers the current font's glyphs within availableHeight.
    int16_t centeredBaseline(U8G2_FOR_ADAFRUIT_GFX &fonts, int16_t availableHeight)
    {
        int8_t ascent = fonts.getFontAscent();
        int8_t descent = fonts.getFontDescent(); // <= 0
        int16_t fontHeight = ascent - descent;
        return (availableHeight - fontHeight) / 2 + ascent;
    }
} // namespace

void HeaderBar::draw(Display &display, uint8_t batteryPercent, bool charging)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    if (batteryPercent > 100)
        batteryPercent = 100;

    int16_t panelW = gfx.width();
    int16_t contentH = HEIGHT - 1; // excludes the bottom rule

    fonts.setFont(u8g2_font_siji_t_6x10);
    int16_t iconBaseline = centeredBaseline(fonts, contentH);

    uint8_t level = (uint8_t)((batteryPercent * (BATTERY_GLYPH_COUNT - 1) + 50) / 100);
    uint16_t batteryGlyph = BATTERY_GLYPH_MIN + level;
    int16_t batteryW = u8g2_GetGlyphWidth(&fonts.u8g2, batteryGlyph);
    int16_t batteryX = panelW - MARGIN_RIGHT - batteryW;
    fonts.drawGlyph(batteryX, iconBaseline, batteryGlyph);

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t textBaseline = centeredBaseline(fonts, contentH);
    char label[5];
    snprintf(label, sizeof(label), "%u%%", batteryPercent);
    int16_t textW = fonts.getUTF8Width(label);
    int16_t textX = batteryX - GAP - textW;
    fonts.setCursor(textX, textBaseline);
    fonts.print(label);

    if (charging)
    {
        fonts.setFont(u8g2_font_siji_t_6x10);
        int16_t chargingW = u8g2_GetGlyphWidth(&fonts.u8g2, CHARGING_GLYPH);
        int16_t chargingX = textX - GAP - chargingW;
        fonts.drawGlyph(chargingX, iconBaseline, CHARGING_GLYPH);
    }

    gfx.drawFastHLine(0, HEIGHT - 1, panelW, GxEPD_BLACK);
}
