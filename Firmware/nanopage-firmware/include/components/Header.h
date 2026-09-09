// Header bar: current directory (left), battery + charge status (right).

#pragma once

#include <cstdint>

class Display;

class Header
{
public:
    static constexpr int16_t HEIGHT = 16; // must be a multiple of 8 - see Display::refreshPartial()

    // Caches percent/charging without touching the display - call once before
    // the very first draw() so it already has real state to show.
    void setBatteryState(float percent, bool charging)
    {
        percent_ = percent;
        charging_ = charging;
    }

    // Draws the full header band: `path` on the left, battery/charging on the
    // right. Called by whoever owns the surrounding full/partial refresh -
    // currently FileBrowser, since navigating is what changes the path.
    void draw(Display &display, const char *path);

    // Updates cached percent/charging and repaints just the battery region
    // (the right-hand BATTERY_REGION_WIDTH px) as its own partial refresh -
    // safe independent of whatever else is on screen, since the header
    // occupies rows the rest of the UI never draws into.
    void updateBattery(Display &display, float percent, bool charging);

private:
    // siji icon codepoints. Battery glyphs are a 9-step ramp, empty to full.
    static constexpr uint16_t ICON_USB = 0xe1b3;
    static constexpr uint16_t ICON_BATTERY_EMPTY = 0xe24c;
    static constexpr uint16_t ICON_BATTERY_FULL = 0xe254;
    static constexpr uint8_t BATTERY_LEVELS = ICON_BATTERY_FULL - ICON_BATTERY_EMPTY + 1;

    // Right-hand slice redrawn by updateBattery() - must be a multiple of 8,
    // and leave (screen width - this value) a multiple of 8 too, per
    // Display::refreshPartial()'s alignment rule. Screen is 296px wide.
    static constexpr int16_t BATTERY_REGION_WIDTH = 96;
    static constexpr int16_t MARGIN = 2;
    static constexpr int16_t PERCENT_BATTERY_GAP = 3; // extra space between "%" and the battery glyph

    float percent_ = 0.0f;
    bool charging_ = false;

    void drawPath(Display &display, const char *path);
    void drawBattery(Display &display);
};
