// GDEY029T94 e-paper panel (SSD1680, 128x296, SPI). Wraps GxEPD2 plus
// U8g2_for_Adafruit_GFX for header-bar-sized text.

#pragma once

#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

#include "Config.h"

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
    // Separately: a display-garbling bug (repro: boot, move the cursor down
    // 11 times - garbles at the 2nd full-after-partial transition) needs
    // BOTH this gap AND displayCorruptionWorkaroundPadding in Display.cpp -
    // confirmed on hardware that removing either one while the other is in
    // place brings the exact same corruption straight back. See
    // project_display_usb_cdc_refresh_corruption memory before touching
    // either one. This gap alone: starting a refresh too soon after the
    // panel's own last hibernate() contributes to the corruption.
    // MIN_REFRESH_GAP_MS has NOT been hardware-bisected to find the true
    // minimum, just picked with margin above what incidentally worked.
    static constexpr uint32_t MIN_REFRESH_GAP_MS = 30;

    template <typename DrawFn>
    void refreshFull(DrawFn draw)
    {
        waitForMinRefreshGap();
        panel_.setFullWindow();
        panel_.firstPage();
        do
        {
            panel_.fillScreen(GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
        lastRefreshEndMs_ = millis();
        partialRefreshCount_ = 0;
    }

    // Fast partial update (~400ms vs ~1.75s for a full refresh on this
    // panel) - only pushes the given rect, so draw() must fully repaint it.
    // x and y (and so w and h) must be multiples of 8: this panel's Y axis
    // is byte-addressed under setRotation(3), and GxEPD2 rounds an
    // unaligned window outward on both edges - since this buffer is fully
    // wiped white on every refresh rather than persisting prior content,
    // whatever that rounding pulls in beyond what draw() repaints comes out
    // blank. Callers must keep their own layout 8-aligned rather than
    // compensating here (tried and reverted - interacted badly with the
    // panel's fast partial-update waveform tracking).
    template <typename DrawFn>
    void refreshPartial(int16_t x, int16_t y, int16_t w, int16_t h, DrawFn draw)
    {
        waitForMinRefreshGap();
        panel_.setPartialWindow(x, y, w, h);
        panel_.firstPage();
        do
        {
            panel_.fillRect(x, y, w, h, GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
        lastRefreshEndMs_ = millis();
    }

    // Global partial-refresh counter shared by all callers - forces a full
    // refresh (via fullDraw) every Config::FULL_REFRESH_EVERY calls.
    template <typename PartialDrawFn, typename FullDrawFn>
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h, PartialDrawFn partialDraw, FullDrawFn fullDraw)
    {
        if (++partialRefreshCount_ >= Config::FULL_REFRESH_EVERY)
            refreshFull(fullDraw);
        else
            refreshPartial(x, y, w, h, partialDraw);
    }

    Panel &panel() { return panel_; }
    U8G2_FOR_ADAFRUIT_GFX &fonts() { return fonts_; }

private:
    Panel panel_;
    U8G2_FOR_ADAFRUIT_GFX fonts_;
    uint16_t partialRefreshCount_ = 0;
    uint32_t lastRefreshEndMs_ = 0;

    // Blocks until at least MIN_REFRESH_GAP_MS has passed since the panel's
    // last hibernate() - see the note on MIN_REFRESH_GAP_MS above.
    void waitForMinRefreshGap()
    {
        uint32_t elapsed = millis() - lastRefreshEndMs_; // wraps correctly even across a millis() rollover
        if (elapsed < MIN_REFRESH_GAP_MS)
            delay(MIN_REFRESH_GAP_MS - elapsed);
    }
};
