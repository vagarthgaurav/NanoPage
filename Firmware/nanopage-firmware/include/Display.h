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
    // Separately: a display-garbling bug (repro: run enough consecutive
    // partial refreshes to trigger a full one) went through several wrong
    // explanations before landing here - see project_display_usb_cdc_refresh_corruption
    // memory for the full history, including a RAM-padding reservation and an
    // earlier untested 30ms gap that both turned out to be red herrings.
    // 2026-09-09: the real tell was that the cursor visibly slows down for
    // several presses *before* a corrupting full refresh - not what software
    // memory corruption looks like. That points at the panel's own charge
    // pump not fully recovering between refreshes issued too close together,
    // building up over a run of partial refreshes until the full refresh
    // (which draws more current) exposes it as a bad waveform. Gating only
    // the full refresh with a pre-delay (not every partial one) fixed it,
    // hardware-confirmed at 10ms with no padding array needed.
    //
    // Bumped to 200 and back down again on 2026-09-10 while chasing what
    // turned out to be a different bug (a periodic Serial print burst - the
    // fuel-gauge register dump - landing mid-refresh; see
    // project_sd_card_backpowering_idle_current memory). The gap value was
    // never actually the problem that round.
    static constexpr uint32_t MIN_REFRESH_GAP_MS = 10;

    template <typename DrawFn>
    void refreshFull(DrawFn draw)
    {
        deselectSdCard();
        waitForMinRefreshGap();
        panel_.setFullWindow();
        panel_.firstPage();
        do
        {
            panel_.fillScreen(GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
        parkSdCardCsLow();
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
        deselectSdCard();
        panel_.setPartialWindow(x, y, w, h);
        panel_.firstPage();
        do
        {
            panel_.fillRect(x, y, w, h, GxEPD_WHITE);
            draw();
        } while (panel_.nextPage());
        panel_.hibernate();
        parkSdCardCsLow();
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

    // SD_CS idles low between accesses (see SdCardStorage::listDir()) to
    // stop it backpowering the unpowered card through its own clamp diode -
    // but that pin is on the same shared SCK/MOSI/MISO bus this panel uses.
    // Deselecting it defensively before every panel transfer and parking it
    // low again after fixed a silent boot-time refresh failure this caused
    // when left unguarded.
    //
    // A run of apparent display regressions (ghosting, a stuck cursor) got
    // blamed on this guard while it was being tuned on 2026-09-10, including
    // a full revert-and-rebuild cycle. CONFIRMED afterward those were a
    // separate, unrelated bug - a periodic Serial print burst
    // (Config::DEBUG_REGISTER_DUMP_INTERVAL_MS) landing mid-refresh, clean
    // A/B'd on hardware independent of this guard. See
    // project_sd_card_backpowering_idle_current memory for the full history
    // if something in this area breaks again - don't re-blame this guard
    // without first checking whether DEBUG_REGISTER_DUMP_INTERVAL_MS (or any
    // other periodic Serial print) is active.
    void deselectSdCard() { digitalWrite(PIN_SD_CS, HIGH); }
    void parkSdCardCsLow() { digitalWrite(PIN_SD_CS, LOW); }
};
