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

        // CS idles low while unpowered (see below) to stop it backpowering
        // the card through its own clamp diode - but the SD-over-SPI init
        // sequence needs CS deselected (high) while it clocks the card's
        // mandatory post-power-up dummy cycles, or the card never drops into
        // SPI mode and sd.begin() hangs forever.
        digitalWrite(PIN_SD_CS, HIGH);

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

        // Cut power first, then wait, then deselect - not the other way
        // around. The TPS22929D's output doesn't discharge instantly once
        // SD_ON goes low, and pulling CS low while the card is still
        // partially powered selects a not-yet-dead chip - a half-powered,
        // selected card loading SCK/MOSI risked corrupting whatever transfer
        // came next on the shared bus.
        digitalWrite(PIN_SD_ON, LOW);
        delay(50); // let the load switch's output capacitance discharge
        digitalWrite(PIN_SD_CS, LOW);

        return mounted;
    }
} // namespace SdCardStorage
