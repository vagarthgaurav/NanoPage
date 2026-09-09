// microSD card access (SdFat). Shares the SPI bus with the e-paper panel.

#pragma once

#include <cstdint>

namespace SdCardStorage
{
    constexpr uint8_t MAX_ENTRIES = 32;
    constexpr uint8_t MAX_NAME_LEN = 40;

    struct Entry
    {
        char name[MAX_NAME_LEN];
        bool isDir;
    };

    // Powers the card, mounts it, lists `path` into entries (up to
    // maxEntries), then powers it back down. Returns false if the card
    // can't be mounted; entryCount is 0 in that case.
    bool listDir(const char *path, Entry *entries, uint8_t maxEntries, uint8_t &entryCount);
} // namespace SdCardStorage
