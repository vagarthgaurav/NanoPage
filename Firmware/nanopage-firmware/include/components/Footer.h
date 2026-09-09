// Footer bar: debug data only for now - battery voltage/current/charge state.

#pragma once

#include <cstdint>

class Display;

class Footer
{
public:
    static constexpr int16_t HEIGHT = 16; // must be a multiple of 8 - see Display::refreshPartial()

    // Caches voltage/current/charging without touching the display - call once
    // before the very first draw() so it already has real state to show.
    void setBatteryState(float voltageMv, float currentMa, bool charging)
    {
        voltageMv_ = voltageMv;
        currentMa_ = currentMa;
        charging_ = charging;
    }

    // Draws the full footer band using cached state. Called by whoever owns
    // the surrounding full/partial refresh - currently FileBrowser.
    void draw(Display &display);

    // Updates cached voltage/current/charging and repaints the footer as its
    // own partial refresh - safe independent of whatever else is on screen,
    // since the footer occupies rows the rest of the UI never draws into.
    void update(Display &display, float voltageMv, float currentMa, bool charging);

private:
    float voltageMv_ = 0.0f;
    float currentMa_ = 0.0f;
    bool charging_ = false;
};
