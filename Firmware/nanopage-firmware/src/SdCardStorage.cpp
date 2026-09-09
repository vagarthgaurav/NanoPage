#include "SdCardStorage.h"

#include <Arduino.h>
#include <SPI.h>
#include <SdFat.h>

namespace
{
    SdFat sd;
}

namespace SdCardStorage
{
    bool listDir(const char *path, Entry *entries, uint8_t maxEntries, uint8_t &entryCount)
    {
        entryCount = 0;

        digitalWrite(PIN_SD_ON, HIGH);
        delay(10); // let the card's supply rail settle

        bool mounted = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
        if (mounted)
        {
            File dir = sd.open(path);
            if (dir)
            {
                File entry;
                while (entryCount < maxEntries && (entry = dir.openNextFile()))
                {
                    entry.getName(entries[entryCount].name, MAX_NAME_LEN);
                    entries[entryCount].isDir = entry.isDirectory();
                    entry.close();
                    entryCount++;
                }
                dir.close();
            }
        }

        digitalWrite(PIN_SD_ON, LOW);
        return mounted;
    }
} // namespace SdCardStorage
