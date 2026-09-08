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

    // Redraws only the right-hand voltage/current cluster.
    void drawStats(Display &display, float currentMilliAmps, float voltageMilliVolts);
    // Rect wide enough for that cluster in any sign/digit-count combination,
    // right-aligned to the panel edge. Pass to Display::refreshPartial().
    void statsRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h);

    // Redraws only the left-hand free-heap debug readout.
    void drawHeap(Display &display, uint32_t freeHeapBytes);
    // Rect wide enough for that readout in any digit count, left-aligned.
    void heapRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h);
} // namespace FooterBar
