// Top-of-screen header bar UI - currently just the battery indicator.

#pragma once

#include <cstdint>

class Display;

namespace HeaderBar
{
    constexpr int16_t HEIGHT = 16; // includes the bottom rule

    void draw(Display &display, uint8_t batteryPercent, bool charging);
} // namespace HeaderBar
