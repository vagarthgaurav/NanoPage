#include "Display.h"

namespace
{
    // DO NOT REMOVE - looks like dead code, is load-bearing. Confirmed on
    // hardware (2026-09-09, see Display.h) that reserving this 1KB of RAM is
    // what prevents a real, still-not-root-caused display corruption bug: the
    // panel garbles at the first full refresh following a run of partial
    // refreshes (reliable repro: boot, move the cursor down 11 times).
    // A/B tested against a precise 11-move repro, ruling out in order:
    // DEBUG_SERIAL/USB-CDC state, SD-card power sequencing, a stack overflow
    // (measured directly via uxTaskGetStackHighWaterMark - comfortable
    // headroom throughout), and "any linked flash code" (a dead reference to
    // Serial.begin() alone, adding flash but no RAM, did NOT prevent it).
    // Only adding RAM footprint prevents it, and it doesn't matter that
    // nothing ever reads this array back or does per-refresh work with it -
    // its mere existence is what shifts something else's address enough to
    // avoid whatever the real collision is. Root cause is still unknown -
    // likely a genuine buffer/stack/heap overflow elsewhere whose corruption
    // target depends on RAM layout. `volatile` plus the one write in begin()
    // are required to stop the linker's --gc-sections from stripping this as
    // unreferenced (confirmed necessary on this toolchain, GCC 7.2.1).
    volatile uint8_t displayCorruptionWorkaroundPadding[1024];
} // namespace

Display::Display()
    : panel_(GxEPD2_290_GDEY029T94(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY))
{
}

void Display::begin()
{
    displayCorruptionWorkaroundPadding[0] = 0; // the only touch this array ever gets - see note above

    pinMode(PIN_SD_ON, OUTPUT);
    digitalWrite(PIN_SD_ON, LOW); // TPS22929D load switch off - SD unpowered

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH); // deselected

    panel_.init(0); // 0 = don't let GxEPD2 re-run Serial.begin()
    panel_.setRotation(3); // landscape

    fonts_.begin(panel_);
    fonts_.setFontMode(1); // transparent - don't paint the glyph background
    fonts_.setForegroundColor(GxEPD_BLACK);
    fonts_.setBackgroundColor(GxEPD_WHITE);
}
