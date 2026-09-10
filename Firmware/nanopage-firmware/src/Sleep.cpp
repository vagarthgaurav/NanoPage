#include "Sleep.h"

#include <Arduino.h>
#include <Wire.h>

#include "Buttons.h"
#include "Config.h"
#include "Display.h"
#include "FuelGauge.h"

namespace
{
    void drawSleepIcon(Display &display)
    {
        auto &gfx = display.panel();
        int16_t cx = gfx.width() / 2;
        int16_t cy = gfx.height() / 2 - 14;
        int16_t r = 18;

        gfx.fillCircle(cx, cy, r, GxEPD_BLACK);
        gfx.fillCircle(cx + r / 2, cy - r / 3, r, GxEPD_WHITE); // bites a crescent out of the disc

        auto &fonts = display.fonts();
        fonts.setFont(u8g2_font_10x20_tr);
        const char *caption = "NanoPage";
        int16_t textW = fonts.getUTF8Width(caption);
        fonts.setCursor(cx - textW / 2, cy + r + 24);
        fonts.print(caption);
    }
} // namespace

namespace Sleep
{
    void enterDeepSleep(Display &display, FuelGauge &fuelGauge)
    {
        if (Config::DEBUG_SERIAL)
        {
            Serial.println("No activity - entering deep sleep");
            Serial.flush();
        }

        display.refreshFull([&display]()
                             { drawSleepIcon(display); });

        fuelGauge.saveLearnedParamsToFlash();

        Wire.end();

        // Belt-and-suspenders: listDir() already turns this off after every SD
        // access and Display::begin() re-clears it on every boot, but GPIO output
        // state survives SYSTEMOFF - if it were ever still HIGH here, the SD card
        // would stay powered for the entire sleep period.
        if (Config::DEBUG_SERIAL && digitalRead(PIN_SD_ON) == HIGH)
            Serial.println("WARNING: PIN_SD_ON was still HIGH going into deep sleep");
        digitalWrite(PIN_SD_ON, LOW);

        Buttons::armWakeOnPress();

        delay(50); // let SPI/GPIO changes settle before cutting power
        NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
        while (true)
        {
        } // fallback - SYSTEMOFF should cut power before this ever runs
    }
} // namespace Sleep
