// Battery discharge accelerator for MAX17055 calibration - never sleeps, hammers
// the SD card and e-paper panel continuously to draw far more current than normal
// use, so a full charge/discharge learning cycle takes hours instead of days.
// Flash with `pio run -e nanopage_burntest -t upload`; restore normal firmware
// afterwards with `pio run -e nanopage -t upload`.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h>
#include <SPI.h>
#include <SdFat.h>
#include <Wire.h>

#include "Display.h"
#include "FuelGauge.h"

namespace
{
    constexpr uint32_t LOG_INTERVAL_MS = 30000;
    constexpr size_t WRITE_CHUNK_BYTES = 512;
    constexpr uint32_t WRITE_CHUNK_COUNT = 2048; // ~1MB per iteration

    // Neither the BQ25170 nor this bare EEMB cell has any over-discharge cutoff -
    // this is the only thing standing between the load and a damaged battery.
    constexpr float SAFE_STOP_MV = FuelGauge::VEMPTY_MV;

    FuelGauge fuelGauge;
    Display display;
    SdFat sd;
    bool sdMounted = false;
    bool stopped = false;
    uint8_t scratch[WRITE_CHUNK_BYTES];

    void drawStatus(uint32_t elapsedS, float percent, float mv, float ma)
    {
        auto &fonts = display.fonts();
        fonts.setFont(u8g2_font_10x20_tr);
        fonts.setCursor(4, 30);
        fonts.print("BURN TEST");

        fonts.setFont(u8g2_font_6x10_tr);
        char line[40];
        snprintf(line, sizeof(line), "elapsed  %lus", (unsigned long)elapsedS);
        fonts.setCursor(4, 60);
        fonts.print(line);
        snprintf(line, sizeof(line), "SOC      %.1f%%", percent);
        fonts.setCursor(4, 74);
        fonts.print(line);
        snprintf(line, sizeof(line), "voltage  %.0fmV", mv);
        fonts.setCursor(4, 88);
        fonts.print(line);
        snprintf(line, sizeof(line), "current  %.1fmA", ma);
        fonts.setCursor(4, 102);
        fonts.print(line);
    }

    // Repeated write+read+delete of a scratch file keeps the card's flash
    // controller active, which is where most of an SD card's current goes.
    void hammerSdCard()
    {
        if (!sdMounted)
            return;

        File f = sd.open("/burn.bin", O_WRITE | O_CREAT | O_TRUNC);
        if (f)
        {
            for (uint32_t i = 0; i < WRITE_CHUNK_COUNT; i++)
                f.write(scratch, sizeof(scratch));
            f.close();
        }

        File fr = sd.open("/burn.bin", O_READ);
        if (fr)
        {
            while (fr.available())
                fr.read(scratch, sizeof(scratch));
            fr.close();
        }

        sd.remove("/burn.bin");
    }
} // namespace

void setup()
{
    Serial.begin(115200);
    uint32_t waited_ms = 0;
    while (!Serial && waited_ms < 3000)
    {
        delay(10);
        waited_ms += 10;
    }

    Serial.println("=== NanoPage battery burn test ===");
    Serial.println("Continuously hammers SD + e-paper, never sleeps. Do not use for anything else.");

    Wire.begin();
    Wire.setClock(100000);
    if (!fuelGauge.begin())
        Serial.println("Fuel gauge: not found at 0x36");

    display.begin();

    digitalWrite(PIN_SD_ON, HIGH);
    delay(10);
    sdMounted = sd.begin(SdSpiConfig(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(4), &SPI));
    if (!sdMounted)
        Serial.println("SD card: not found - discharge current will be lower without it");

    for (size_t i = 0; i < sizeof(scratch); i++)
        scratch[i] = (uint8_t)i;

    Serial.print("Starting SOC: ");
    Serial.print(fuelGauge.percent());
    Serial.println("%");
}

void loop()
{
    static uint32_t startMs = millis();
    static uint32_t lastLogMs = 0;

    float mv = fuelGauge.voltageMilliVolts();

    // No PCM on this cell and the BQ25170 has no under-voltage cutoff of its own -
    // stop drawing extra current ourselves once the cell reaches its safe floor.
    if (mv <= SAFE_STOP_MV)
    {
        if (!stopped)
        {
            stopped = true;
            digitalWrite(PIN_SD_ON, LOW); // cut every avoidable mA once we're at the floor
            Serial.print("SAFE STOP: cell reached ");
            Serial.print(mv);
            Serial.println("mV - halting load to protect the battery. Recharge it now.");
        }
        delay(5000);
        return;
    }

    hammerSdCard();

    uint32_t elapsedS = (millis() - startMs) / 1000;
    float percent = fuelGauge.percent();
    float ma = fuelGauge.currentMilliAmps();

    display.refreshFull([&]()
                         { drawStatus(elapsedS, percent, mv, ma); });

    uint32_t now = millis();
    if (now - lastLogMs >= LOG_INTERVAL_MS)
    {
        lastLogMs = now;
        Serial.print(elapsedS);
        Serial.print("s  SOC=");
        Serial.print(percent);
        Serial.print("%  V=");
        Serial.print(mv);
        Serial.print("mV  I=");
        Serial.print(ma);
        Serial.println("mA");
    }
}
