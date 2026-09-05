// Bottom-of-screen footer bar UI - battery voltage/current plus a free-heap debug readout.

#pragma once

#include <cstdint>

class Display;

namespace FooterBar
{
    // Must be a multiple of 8, and so must its y-offset (panel height - HEIGHT) - the
    // SSD1680's partial-window column addressing needs both 8-aligned in this rotation,
    // else GxEPD2 rounds the window and the unaligned rows come out corrupted.
    constexpr int16_t HEIGHT = 16; // includes the top rule

    void draw(Display &display, float currentMilliAmps, float voltageMilliVolts, uint32_t freeHeapBytes);
} // namespace FooterBar
