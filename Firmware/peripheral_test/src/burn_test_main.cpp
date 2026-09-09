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
#include <math.h>

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
    bool depleted = false;      // sticky - SAFE STOP fired, never resume hammering
    bool cycleComplete = false; // sticky - charge taper detected, separate from `depleted`
    bool wasCharging = false;
    uint8_t scratch[WRITE_CHUNK_BYTES];

    void drawStatus(uint32_t elapsedS, float percent, float mv, float ma, float peakMa, uint32_t tookMs)
    {
        auto &fonts = display.fonts();
        fonts.setFont(u8g2_font_10x20_tr);
        fonts.setCursor(4, 30);
        fonts.print("BURN TEST");

        fonts.setFont(u8g2_font_6x10_tr);
        char line[40];
        snprintf(line, sizeof(line), "elapsed  %lus", (unsigned long)elapsedS);
        fonts.setCursor(4, 50);
        fonts.print(line);
        snprintf(line, sizeof(line), "SOC      %.1f%%", percent);
        fonts.setCursor(4, 64);
        fonts.print(line);
        snprintf(line, sizeof(line), "voltage  %.0fmV", mv);
        fonts.setCursor(4, 78);
        fonts.print(line);
        snprintf(line, sizeof(line), "I after  %.1fmA", ma);
        fonts.setCursor(4, 92);
        fonts.print(line);
        snprintf(line, sizeof(line), "I peak   %.1fmA", peakMa);
        fonts.setCursor(4, 106);
        fonts.print(line);
        snprintf(line, sizeof(line), "SD took  %lums", (unsigned long)tookMs);
        fonts.setCursor(4, 120);
        fonts.print(line);
    }

    // Shared layout for the two terminal-event screens below - drawn once, right
    // when each event fires, then left on screen (no more redraws follow in that
    // phase, so there's nothing to refresh again).
    void drawEventStatus(const char *title, uint32_t elapsedS, float percent, float mv)
    {
        auto &fonts = display.fonts();
        fonts.setFont(u8g2_font_10x20_tr);
        fonts.setCursor(4, 30);
        fonts.print(title);

        fonts.setFont(u8g2_font_6x10_tr);
        char line[40];
        snprintf(line, sizeof(line), "elapsed  %lus", (unsigned long)elapsedS);
        fonts.setCursor(4, 50);
        fonts.print(line);
        snprintf(line, sizeof(line), "SOC      %.1f%%", percent);
        fonts.setCursor(4, 64);
        fonts.print(line);
        snprintf(line, sizeof(line), "voltage  %.0fmV", mv);
        fonts.setCursor(4, 78);
        fonts.print(line);
    }

    void drawDepleted(uint32_t elapsedS, float percent, float mv)
    {
        drawEventStatus("DEPLETED", elapsedS, percent, mv);
        auto &fonts = display.fonts();
        fonts.setCursor(4, 92);
        fonts.print("discharge done");
        fonts.setCursor(4, 106);
        fonts.print("recharge now");
    }

    void drawChargeComplete(uint32_t elapsedS, float percent, float mv)
    {
        drawEventStatus("CHARGED", elapsedS, percent, mv);
        auto &fonts = display.fonts();
        fonts.setCursor(4, 92);
        fonts.print("cycle complete");
        fonts.setCursor(4, 106);
        fonts.print("reflash normal fw");
    }

    // Repeated write+read+delete of a scratch file keeps the card's flash
    // controller active, which is where most of an SD card's current goes.
    // Samples current mid-transfer too, since a single post-hoc read can miss
    // the actual load if the card is slow enough that most loop time is idle.
    // Returns {peak |current| seen, wall-clock ms spent}.
    void hammerSdCard(float &peakMa, uint32_t &tookMs)
    {
        peakMa = 0.0f;
        uint32_t startMs = millis();
        if (!sdMounted)
        {
            tookMs = 0;
            return;
        }

        auto sample = [&]()
        {
            float ma = fabsf(fuelGauge.currentMilliAmps());
            if (ma > peakMa)
                peakMa = ma;
        };

        File f = sd.open("/burn.bin", O_WRITE | O_CREAT | O_TRUNC);
        if (f)
        {
            for (uint32_t i = 0; i < WRITE_CHUNK_COUNT; i++)
            {
                f.write(scratch, sizeof(scratch));
                if (i % 64 == 0)
                    sample();
            }
            f.close();
        }
        sample();

        File fr = sd.open("/burn.bin", O_READ);
        if (fr)
        {
            uint32_t i = 0;
            while (fr.available())
            {
                fr.read(scratch, sizeof(scratch));
                if (i++ % 64 == 0)
                    sample();
            }
            fr.close();
        }
        sample();

        sd.remove("/burn.bin");
        tookMs = millis() - startMs;
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
    float percent = fuelGauge.percent();
    float ma = fuelGauge.currentMilliAmps();
    bool charging = fuelGauge.isCharging();
    uint32_t elapsedS = (millis() - startMs) / 1000;

    // A charge-completion edge (was charging, now not, at a real full-charge
    // voltage) means the learning cycle is done - stop hammering for good so we
    // don't silently kick off another discharge the user didn't ask for. Gated
    // only on its own sticky flag (not on `depleted`) so this can still fire
    // after an earlier SAFE STOP - a full run normally hits both events in turn.
    if (wasCharging && !charging && mv > 4000.0f && !cycleComplete)
    {
        cycleComplete = true;
        Serial.println("CYCLE COMPLETE: charge tapered off after a full discharge/charge cycle.");
        Serial.println("Reflash normal firmware with `pio run -e nanopage -t upload` and check the SOC reading.");
        fuelGauge.debugDumpRegisters();
        display.refreshFull([&]()
                             { drawChargeComplete(elapsedS, percent, mv); });
    }
    wasCharging = charging;

    // No PCM check needed here (the pack has one), but the BQ25170 has no
    // under-voltage cutoff of its own - stop drawing extra current ourselves
    // once the cell reaches its safe floor.
    if (!depleted && mv <= SAFE_STOP_MV)
    {
        depleted = true;
        digitalWrite(PIN_SD_ON, LOW); // cut every avoidable mA once we're at the floor
        Serial.print("SAFE STOP: cell reached ");
        Serial.print(mv);
        Serial.println("mV - halting load to protect the battery. Recharge it now.");
        fuelGauge.debugDumpRegisters();
        display.refreshFull([&]()
                             { drawDepleted(elapsedS, percent, mv); });
    }

    // Never add load while the charger is trying to reach a clean, tapering
    // full charge - that's the signal the fuel gauge needs to re-anchor to 100%.
    if (depleted || cycleComplete || charging)
    {
        uint32_t now = millis();
        if (now - lastLogMs >= LOG_INTERVAL_MS)
        {
            lastLogMs = now;
            Serial.print(elapsedS);
            Serial.print("s  (idle, charging=");
            Serial.print(charging ? "true" : "false");
            Serial.print(")  SOC=");
            Serial.print(percent);
            Serial.print("%  V=");
            Serial.print(mv);
            Serial.print("mV  I=");
            Serial.print(ma);
            Serial.println("mA");
        }
        delay(2000);
        return;
    }

    float peakMa = 0.0f;
    uint32_t tookMs = 0;
    hammerSdCard(peakMa, tookMs);

    display.refreshFull([&]()
                         { drawStatus(elapsedS, percent, mv, ma, peakMa, tookMs); });

    uint32_t now = millis();
    if (now - lastLogMs >= LOG_INTERVAL_MS)
    {
        lastLogMs = now;
        Serial.print(elapsedS);
        Serial.print("s  SOC=");
        Serial.print(percent);
        Serial.print("%  V=");
        Serial.print(mv);
        Serial.print("mV  I(after)=");
        Serial.print(ma);
        Serial.print("mA  I(peak during SD)=");
        Serial.print(peakMa);
        Serial.print("mA  SD hammer took=");
        Serial.print(tookMs);
        Serial.println("ms");
    }
}
