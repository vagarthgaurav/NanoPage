// NanoPage main firmware. Pull in peripheral_test's RTC/microSD/BLE code as this grows.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // pulls in the USB CDC Serial object; nothing links it in otherwise
#include <Wire.h>
#include <semphr.h>

#include "Config.h"
#include "Display.h"
#include "FileBrowser.h"
#include "FuelGauge.h"
#include "HeaderBar.h"

static FuelGauge fuelGauge;
static Display display;
static FileBrowser fileBrowser;

// loop() and the button ISR_DEFERRED task both call into display/fileBrowser - guard against overlap.
static SemaphoreHandle_t displayMutex;

// Sentinels guarantee the first call always draws.
static uint8_t lastPercent = 255;
static bool lastCharging = false;
static uint8_t refreshCount = 0;

// refreshFull() clears the whole panel, so a full refresh must redraw both regions.
static void drawHeaderAndContent()
{
    HeaderBar::draw(display, lastPercent, lastCharging, fileBrowser.currentPath());
    fileBrowser.draw(display);
}

static void redrawHeader(bool force = false)
{
    uint8_t percent = (uint8_t)fuelGauge.percent();
    float currentMa = fuelGauge.currentMilliAmps();
    bool charging = fuelGauge.isCharging();

    if (!force && percent == lastPercent && charging == lastCharging)
        return; // nothing changed - skip the e-paper refresh entirely

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    lastPercent = percent;
    lastCharging = charging;

    Serial.print("Fuel gauge: ");
    Serial.print(percent);
    Serial.print("%, ");
    Serial.print(currentMa);
    Serial.print("mA, charging=");
    Serial.println(charging ? "true" : "false");

    bool full = refreshCount % Config::FULL_REFRESH_EVERY == 0;
    if (full)
        display.refreshFull(drawHeaderAndContent);
    else
        display.refreshPartial(0, 0, display.panel().width(), HeaderBar::HEIGHT, [&]()
                                { HeaderBar::draw(display, percent, charging, fileBrowser.currentPath()); });
    refreshCount++;
    xSemaphoreGive(displayMutex);
}

// includeHeader also redraws the header's path label. Caller must hold displayMutex.
static void redrawFileBrowser(bool includeHeader = false)
{
    bool full = refreshCount % Config::FULL_REFRESH_EVERY == 0;
    if (full)
    {
        display.refreshFull(drawHeaderAndContent);
    }
    else if (includeHeader)
    {
        display.refreshPartial(0, 0, display.panel().width(), display.panel().height(), drawHeaderAndContent);
    }
    else
    {
        int16_t y = HeaderBar::HEIGHT;
        int16_t h = display.panel().height() - y;
        display.refreshPartial(0, y, display.panel().width(), h, [&]()
                                { fileBrowser.draw(display); });
    }
    refreshCount++;
}

static void onPrevEdge()
{
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    fileBrowser.prev();
    redrawFileBrowser();
    xSemaphoreGive(displayMutex);
}

static void onNextEdge()
{
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    fileBrowser.next();
    redrawFileBrowser();
    xSemaphoreGive(displayMutex);
}

static void onActionEdge()
{
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    if (fileBrowser.activateSelected())
        redrawFileBrowser(/*includeHeader=*/true);
    xSemaphoreGive(displayMutex);
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

    displayMutex = xSemaphoreCreateMutex();

    Wire.begin();
    Wire.setClock(100000);

    if (!fuelGauge.begin())
        Serial.println("Fuel gauge: not found at 0x36");

    pinMode(PIN_BTN_PREV, INPUT_PULLUP);
    pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
    pinMode(PIN_BTN_ACTION, INPUT_PULLUP);
    attachInterrupt(PIN_BTN_PREV, onPrevEdge, FALLING | ISR_DEFERRED);
    attachInterrupt(PIN_BTN_NEXT, onNextEdge, FALLING | ISR_DEFERRED);
    attachInterrupt(PIN_BTN_ACTION, onActionEdge, FALLING | ISR_DEFERRED);

    display.begin();

    // Only FileBrowser is wired up so far - see PageMode.h.
    if (!fileBrowser.begin())
        Serial.println("SD card: not found");

    redrawHeader();
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    redrawFileBrowser();
    xSemaphoreGive(displayMutex);
}

void loop()
{
    delay(Config::FUEL_GAUGE_POLL_INTERVAL_MS); // lets FreeRTOS tickless idle sleep instead of busy-polling
    redrawHeader();
}
