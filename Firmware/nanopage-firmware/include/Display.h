// GDEY029T94 e-paper panel (SSD1680, 128x296, SPI). Wraps GxEPD2 plus
// U8g2_for_Adafruit_GFX for header-bar-sized text.

#pragma once

#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

class Display
{
public:
    using Panel = GxEPD2_BW<GxEPD2_290_GDEY029T94, GxEPD2_290_GDEY029T94::HEIGHT>;

    Display();

    // SPI is shared with the microSD card, so its CS/power-enable are
    // driven inactive first to keep it off the bus while the panel inits.
    void begin();

    // panel_.hibernate() after every refresh (not just powerOff(), and not just
    // on the full path) is confirmed by testing to be required on this board -
    // removing it from refreshPartial() made the very first partial refresh
    // garble immediately, twice, despite GxEPD2's own docs suggesting the
    // opposite (that a reset before a partial update without an intervening
    // full one should be what causes corruption). Whatever the real mechanism
    // is here, it isn't that - so don't "fix" this again without a fresh test.
    //
    // Garbling still shows up every ~8-9 refreshes even with hibernate() kept,
    // and only when DEBUG_SERIAL is off. A settleDelay() after hibernate() (extra
    // idle time before the next refresh) did NOT fix it, even with the user
    // manually waiting well over that delay between presses - so it isn't a
    // timing/settling issue. It's count-based, not time-based, and it lines up
    // with Config::FULL_REFRESH_EVERY - worth checking whether something about
    // switching between full-window and partial-window mode on the *same*
    // shared _buffer (see GxEPD2_BW) is the actual cause.
    template <typename DrawFn>
    void refreshFull(DrawFn draw)
    {
        panel_.setFullWindow();
        panel_.firstPage();
        do
        {
            panel_.fillScreen(GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
    }

    // Fast partial update (~400ms vs ~1.75s for a full refresh on this
    // panel) - only pushes the given rect, so draw() must fully repaint it.
    template <typename DrawFn>
    void refreshPartial(int16_t x, int16_t y, int16_t w, int16_t h, DrawFn draw)
    {
        panel_.setPartialWindow(x, y, w, h);
        panel_.firstPage();
        do
        {
            panel_.fillRect(x, y, w, h, GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
    }

    Panel &panel() { return panel_; }
    U8G2_FOR_ADAFRUIT_GFX &fonts() { return fonts_; }

private:
    Panel panel_;
    U8G2_FOR_ADAFRUIT_GFX fonts_;
};
