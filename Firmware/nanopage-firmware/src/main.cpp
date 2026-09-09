// NanoPage main firmware. Pull in peripheral_test's RTC/microSD/BLE code as this grows.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // pulls in the USB CDC Serial object; nothing links it in otherwise
#include <Wire.h>

#include "Buttons.h"
#include "Charger.h"
#include "Config.h"
#include "Display.h"
#include "FuelGauge.h"
#include "Sleep.h"
#include "components/FileBrowser.h"

static FuelGauge fuelGauge;
static Display display;
static FileBrowser fileBrowser;

// Shared with the fuel-gauge ALRT handler below, which resets this on a serviced
// alert so the fallback poll timer counts from the last real update either way.
static uint32_t lastFuelGaugePoll = 0;

void setup()
{
    if (Config::DEBUG_SERIAL)
    {
        Serial.begin(115200);
        uint32_t waited_ms = 0;
        while (!Serial && waited_ms < 3000)
        {
            delay(10);
            waited_ms += 10;
        }

        Serial.println("NanoPage firmware booting");
    }

    Wire.begin();
    Wire.setClock(100000);

    fuelGauge.beginWithSavedState();

    // Fires the instant charging starts or stops (USB plugged in, unplugged, or
    // charge complete), independent of the fuel gauge's own current reading -
    // PIN_CHG_STAT is what Charger::isCharging() reads. Configures that pin,
    // so must run before anything else reads isCharging().
    Charger::begin([]()
                   {
        fuelGauge.logStatus(Charger::isCharging(), Charger::isPowerGood());
        fileBrowser.updateBattery(display, fuelGauge.percent(), Charger::isCharging()); });

    fileBrowser.setBatteryState(fuelGauge.percent(), Charger::isCharging());

    // MAX17055 ALRT fires on every ~1% RepSOC change (see FuelGauge::enableSocChangeAlert).
    // Reset the fallback poll timer here too, so it counts from the last real update either way.
    fuelGauge.enableAlertInterrupt([]()
                                   {
        lastFuelGaugePoll = millis();
        fuelGauge.logStatus(Charger::isCharging(), Charger::isPowerGood());
        fileBrowser.updateBattery(display, fuelGauge.percent(), Charger::isCharging()); });

    display.begin();

    // Also clears the panel of whatever was last drawn before reset/power loss.
    fileBrowser.begin(display);

    Buttons::begin(
        []()
        { fileBrowser.prev(display); },
        []()
        { fileBrowser.next(display); },
        []()
        { fileBrowser.activate(display); });
}

void loop()
{
    static uint32_t lastFuelGaugeSave = 0;
    delay(1000); // lets FreeRTOS tickless idle sleep instead of busy-polling

    uint32_t now = millis();
    if (now - lastFuelGaugePoll >= Config::FUEL_GAUGE_POLL_INTERVAL_MS)
    {
        lastFuelGaugePoll = now;
        fuelGauge.logStatus(Charger::isCharging(), Charger::isPowerGood());
        fileBrowser.updateBattery(display, fuelGauge.percent(), Charger::isCharging());
    }

    if (now - lastFuelGaugeSave >= Config::FUEL_GAUGE_SAVE_INTERVAL_MS)
    {
        lastFuelGaugeSave = now;
        fuelGauge.saveLearnedParamsToFlash();
    }

    if (Buttons::msSinceActivity() >= Config::SLEEP_TIMEOUT_MS)
        Sleep::enterDeepSleep(display, fuelGauge);
}
