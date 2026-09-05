// NanoPage main firmware. Pull in peripheral_test's RTC/microSD/BLE code as this grows.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // pulls in the USB CDC Serial object; nothing links it in otherwise
#include <Wire.h>

#include "Display.h"
#include "FuelGauge.h"
#include "HeaderBar.h"

static FuelGauge fuelGauge;
static Display display;

// Sentinels guarantee the first call always draws.
static uint8_t lastPercent = 255;
static bool lastCharging = false;
static uint8_t refreshCount = 0;
static const uint8_t FULL_REFRESH_EVERY = 8; // partial updates ghost - clear it periodically

static void redraw()
{
    uint8_t percent = (uint8_t)fuelGauge.percent();
    float currentMa = fuelGauge.currentMilliAmps();
    bool charging = fuelGauge.isCharging();

    if (percent == lastPercent && charging == lastCharging)
        return; // nothing changed - skip the e-paper refresh entirely
    lastPercent = percent;
    lastCharging = charging;

    Serial.print("Fuel gauge: ");
    Serial.print(percent);
    Serial.print("%, ");
    Serial.print(currentMa);
    Serial.print("mA, charging=");
    Serial.println(charging ? "true" : "false");

    auto drawHeader = [&]()
    { HeaderBar::draw(display, percent, charging); };

    if (refreshCount % FULL_REFRESH_EVERY == 0)
        display.refreshFull(drawHeader);
    else
        display.refreshPartial(0, 0, display.panel().width(), HeaderBar::HEIGHT, drawHeader);
    refreshCount++;
}

void setup()
{
    Serial.begin(115200);
    uint32_t waited_ms = 0;
    while (!Serial && waited_ms < 3000)
    {
        delay(10);
        waited_ms += 10;
    }

    Serial.println("NanoPage firmware booting");

    Wire.begin();
    Wire.setClock(100000);

    if (!fuelGauge.begin())
        Serial.println("Fuel gauge: not found at 0x36");

    display.begin();
    redraw();
}

void loop()
{
    static const uint32_t POLL_INTERVAL_MS = 5000; // fuel gauge poll only - redraw() itself skips unchanged state

    delay(POLL_INTERVAL_MS); // lets FreeRTOS tickless idle sleep instead of busy-polling
    redraw();
}
