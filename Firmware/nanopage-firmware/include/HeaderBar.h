// Top-of-screen header bar UI - battery indicator plus an optional left-aligned label.

#pragma once

#include <cstdint>

class Display;

namespace HeaderBar
{
    constexpr int16_t HEIGHT = 16; // includes the bottom rule

    void draw(Display &display, uint8_t batteryPercent, bool charging, const char *label = nullptr);

    // Redraws only the right-hand battery/%/charging cluster, not the label -
    // use when just percent/charging changed, to avoid a full-header refresh.
    void drawBatteryCluster(Display &display, uint8_t batteryPercent, bool charging);

    // Rect covering the widest possible extent of that cluster (any percent,
    // charging or not), right-aligned to the panel edge. Pass straight to
    // Display::refreshPartial() before calling drawBatteryCluster() - it's
    // wide enough to also clear whatever the cluster looked like before,
    // regardless of which state (old or new) was wider.
    void batteryClusterRect(Display &display, int16_t &x, int16_t &y, int16_t &w, int16_t &h);
} // namespace HeaderBar
