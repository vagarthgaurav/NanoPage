// SD-card file/folder listing screen.

#pragma once

#include <cstdint>

#include "SdCardStorage.h"

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

private:
    static constexpr uint8_t MAX_PATH_LEN = 128;

    SdCardStorage::Entry entries_[SdCardStorage::MAX_ENTRIES];
    uint8_t entryCount_ = 0;
    uint8_t selected_ = 0;
    uint8_t scrollTop_ = 0;
    char path_[MAX_PATH_LEN] = "/";
    bool mounted_ = false;

    void reload();
    void moveSelection(Display &display, uint8_t newSelected);
    void drawAll(Display &display);
    void drawRow(Display &display, uint8_t row);
};
