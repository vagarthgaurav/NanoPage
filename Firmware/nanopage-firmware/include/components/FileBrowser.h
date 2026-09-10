// SD-card file/folder listing screen.

#pragma once

#include <cstdint>

#include "SdCardStorage.h"
#include "components/Footer.h"
#include "components/Header.h"

class Display;

class FileBrowser
{
public:
    // Lists the root directory and draws it.
    void begin(Display &display);

    void prev(Display &display);
    void next(Display &display);

    // Enters the selected folder, or goes up on "..". No-op on a file.
    void activate(Display &display);

    // Forwarded to the header - see Header::setBatteryState/updateBattery.
    void setBatteryState(float percent, bool charging) { header_.setBatteryState(percent, charging); }
    void updateBattery(Display &display, float percent, bool charging) { header_.updateBattery(display, percent, charging); }

    // Forwarded to the footer - see Footer::setBatteryState/update.
    void setFooterState(float voltageMv, float currentMa, float avgCurrentMa, bool charging) { footer_.setBatteryState(voltageMv, currentMa, avgCurrentMa, charging); }
    void updateFooter(Display &display, float voltageMv, float currentMa, float avgCurrentMa, bool charging) { footer_.update(display, voltageMv, currentMa, avgCurrentMa, charging); }

private:
    static constexpr uint8_t MAX_PATH_LEN = 128;

    SdCardStorage::Entry entries_[SdCardStorage::MAX_ENTRIES];
    uint8_t entryCount_ = 0;
    uint8_t selected_ = 0;
    uint8_t scrollTop_ = 0;
    char path_[MAX_PATH_LEN] = "/";
    bool mounted_ = false;
    Header header_;
    Footer footer_;

    void reload();
    void moveSelection(Display &display, uint8_t newSelected);
    void drawAll(Display &display);
    void drawRow(Display &display, uint8_t row);
};
