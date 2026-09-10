#include "components/Footer.h"

#include <cstdio>

#include "Display.h"

namespace
{
    constexpr int16_t MARGIN = 2;
}

void Footer::draw(Display &display)
{
    auto &gfx = display.panel();
    auto &fonts = display.fonts();

    int16_t y = gfx.height() - HEIGHT;

    fonts.setFont(u8g2_font_6x10_tr);
    int16_t baseline = y + fonts.getFontAscent() + 2;

    char text[48];
    snprintf(text, sizeof(text), "%dmV  %.2fmA (avg %.2fmA) %s",
             (int)voltageMv_, (double)currentMa_, (double)avgCurrentMa_, charging_ ? "CHG" : "DSG");
    fonts.setCursor(MARGIN, baseline);
    fonts.print(text);
}

void Footer::update(Display &display, float voltageMv, float currentMa, float avgCurrentMa, bool charging)
{
    voltageMv_ = voltageMv;
    currentMa_ = currentMa;
    avgCurrentMa_ = avgCurrentMa;
    charging_ = charging;

    auto &gfx = display.panel();
    int16_t y = gfx.height() - HEIGHT;
    display.refreshPartial(0, y, gfx.width(), HEIGHT,
                            [&]()
                            { draw(display); });
}
