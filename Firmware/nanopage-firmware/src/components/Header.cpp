#include "components/Header.h"

#include <cstdio>

#include "Display.h"

void Header::draw(Display &display, const char *path)
{
    drawPath(display, path);
    drawBattery(display);
}

void Header::updateBattery(Display &display, float percent, bool charging)
{
    percent_ = percent;
    charging_ = charging;

    auto &gfx = display.panel();
    int16_t regionX = gfx.width() - BATTERY_REGION_WIDTH;
    display.refreshPartial(regionX, 0, BATTERY_REGION_WIDTH, HEIGHT,
                            [&]()
                            { drawBattery(display); });
}

void Header::drawPath(Display &display, const char *path)
{
    auto &fonts = display.fonts();

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t baseline = fonts.getFontAscent() + 2;
    fonts.setCursor(MARGIN, baseline);
    fonts.print(path);
}

void Header::drawBattery(Display &display)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    float percent = percent_;
    if (percent < 0.0f)
        percent = 0.0f;
    else if (percent > 100.0f)
        percent = 100.0f;

    uint8_t level = (uint8_t)(percent / 100.0f * (BATTERY_LEVELS - 1) + 0.5f);
    if (level >= BATTERY_LEVELS)
        level = BATTERY_LEVELS - 1;
    uint16_t batteryGlyph = ICON_BATTERY_EMPTY + level;

    char percentStr[8];
    snprintf(percentStr, sizeof(percentStr), "%d%%", (int)(percent + 0.5f));

    // Measure everything with the fonts that will actually draw it so the
    // group right-aligns exactly, regardless of siji glyph widths.
    fonts.setFont(u8g2_font_siji_t_6x10);
    int16_t batteryWidth = u8g2_GetGlyphWidth(&fonts.u8g2, batteryGlyph);
    int16_t usbWidth = charging_ ? u8g2_GetGlyphWidth(&fonts.u8g2, ICON_USB) : 0;

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t baseline = fonts.getFontAscent() + 2;
    int16_t textWidth = fonts.getUTF8Width(percentStr);

    int16_t x = gfx.width() - MARGIN - batteryWidth - PERCENT_BATTERY_GAP - textWidth - usbWidth;

    if (charging_)
    {
        fonts.setFont(u8g2_font_siji_t_6x10);
        x += fonts.drawGlyph(x, baseline, ICON_USB);
    }

    fonts.setFont(u8g2_font_6x10_tr);
    fonts.setCursor(x, baseline);
    fonts.print(percentStr);
    x += textWidth + PERCENT_BATTERY_GAP;

    fonts.setFont(u8g2_font_siji_t_6x10);
    fonts.drawGlyph(x, baseline, batteryGlyph);
}
