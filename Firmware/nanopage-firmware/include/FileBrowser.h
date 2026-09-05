// SD-card file/folder listing - the file browser screen mode.

#pragma once

#include <cstdint>

class Display;

class FileBrowser
{
public:
    static constexpr uint8_t MAX_ENTRIES = 32;
    static constexpr uint8_t MAX_NAME_LEN = 40;
    static constexpr uint8_t MAX_PATH_LEN = 128;

    // Powers the SD card, mounts it, and lists the root directory.
    bool begin();

    void next();
    void prev();

    // Enters the selected directory, or goes up on "..". Returns true if the listing changed.
    bool activateSelected();

    void draw(Display &display);

    const char *currentPath() const { return path_; }

private:
    struct Entry
    {
        char name[MAX_NAME_LEN];
        bool isDir;
    };

    Entry entries_[MAX_ENTRIES];
    uint8_t entryCount_ = 0;
    uint8_t selected_ = 0;
    uint8_t scrollTop_ = 0;
    char path_[MAX_PATH_LEN] = "/";
    bool mounted_ = false;

    void listCurrentDir();
    void ensureVisible(uint8_t rowsVisible);
};
