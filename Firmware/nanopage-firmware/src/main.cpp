// NanoPage main firmware. Pull in peripheral_test's RTC/microSD/BLE code as this grows.

#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // pulls in the USB CDC Serial object; nothing links it in otherwise
#include <InternalFileSystem.h>
#include <Wire.h>
#include <semphr.h>
#include <utility/debug.h> // dbgHeapFree() - malloc-backed heap on this core, not FreeRTOS's

#include "Config.h"
#include "Display.h"
#include "FileBrowser.h"
#include "FooterBar.h"
#include "FuelGauge.h"
#include "HeaderBar.h"

static FuelGauge fuelGauge;
static Display display;
static FileBrowser fileBrowser;

namespace
{
    // Fuel-gauge learning lives only in the MAX17055's own registers - if the
    // battery is ever unplugged it forgets everything. Mirroring it to internal
    // flash lets us reload instead of relearning from a blank EZ Config.
    constexpr const char *LEARNED_PARAMS_PATH = "/fuelgauge_learned.bin";

    struct StoredLearnedParams
    {
        FuelGauge::LearnedParams params;
        uint16_t checksum;
    };

    uint16_t checksumOf(const FuelGauge::LearnedParams &p)
    {
        uint16_t sum = 0;
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&p);
        for (size_t i = 0; i < sizeof(p); i++)
            sum += bytes[i];
        return sum;
    }

    bool loadLearnedParams(FuelGauge::LearnedParams &out)
    {
        using Adafruit_LittleFS_Namespace::FILE_O_READ;
        Adafruit_LittleFS_Namespace::File f = InternalFS.open(LEARNED_PARAMS_PATH, FILE_O_READ);
        if (!f)
            return false;

        StoredLearnedParams stored;
        bool ok = f.read(&stored, sizeof(stored)) == (int)sizeof(stored);
        f.close();
        if (!ok || checksumOf(stored.params) != stored.checksum)
            return false;
        out = stored.params;
        return true;
    }

    void saveLearnedParams(const FuelGauge::LearnedParams &p)
    {
        using Adafruit_LittleFS_Namespace::FILE_O_WRITE;
        StoredLearnedParams stored{p, checksumOf(p)};
        Adafruit_LittleFS_Namespace::File f = InternalFS.open(LEARNED_PARAMS_PATH, FILE_O_WRITE);
        if (!f)
            return;
        f.write(reinterpret_cast<const uint8_t *>(&stored), sizeof(stored));
        f.close();
    }

    // Reference snapshot from a calibrated board with the same EEMB LP402535 pack -
    // seeds a brand-new chip (no flash save yet either) closer to correct than the
    // generic built-in model, then normal use self-corrects it from there. See
    // README.md "Fuel gauge factory seed" for where these values came from and how
    // to recapture them on a new reference board.
    // Order: rcomp0, tempCo, fullCapNom, fullCapRep, cycles.
    constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x0013, 0x223E, 0x02E6, 0x0280, 0x0013};
    constexpr bool FACTORY_SEED_VALID = true;
} // namespace

// loop() and the button ISR_DEFERRED task both call into display/fileBrowser - guard against overlap.
static SemaphoreHandle_t displayMutex;

// Sentinels guarantee the first call always draws.
static uint8_t lastPercent = 255;
static bool lastCharging = false;
static float lastCurrentMa = 0.0f;
static float lastVoltageMv = 0.0f;
static int16_t lastFooterCurrentRounded = INT16_MIN;
static int16_t lastFooterVoltageCentiV = -1;
static int32_t lastFooterHeapKb = -1;
static uint16_t partialRefreshCount = 0;

static volatile uint32_t lastActivityMs = 0;

static int16_t roundToInt16(float v)
{
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

// Shared across every narrow partial-refresh call site below (header, footer,
// cursor move) - fast partial updates ghost over time, so this forces a full
// clear periodically regardless of which region asks. Every candidate partial
// refresh counts, whichever region it's for.
static bool dueForFullRefresh()
{
    bool due = partialRefreshCount++ % Config::FULL_REFRESH_EVERY == 0;
    if (due)
        Serial.println("Periodic full refresh (ghosting clear)");
    return due;
}

// refreshFull() clears the whole panel, so a full refresh must redraw all three regions.
static void drawHeaderAndContent()
{
    HeaderBar::draw(display, lastPercent, lastCharging, fileBrowser.currentPath());
    fileBrowser.draw(display);
    FooterBar::draw(display, lastCurrentMa, lastVoltageMv, (uint32_t)dbgHeapFree());
}

static void redrawHeader(bool force = false)
{
    uint8_t percent = (uint8_t)fuelGauge.percent();
    bool charging = fuelGauge.isCharging();

    if (!force && percent == lastPercent && charging == lastCharging)
        return; // nothing changed - skip the e-paper refresh entirely

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    lastPercent = percent;
    lastCharging = charging;

    Serial.print("Fuel gauge: ");
    Serial.print(percent);
    Serial.print("%, charging=");
    Serial.println(charging ? "true" : "false");

    if (dueForFullRefresh())
    {
        display.refreshFull(drawHeaderAndContent);
    }
    else
    {
        int16_t x, y, w, h;
        HeaderBar::batteryClusterRect(display, x, y, w, h);
        display.refreshPartial(x, y, w, h, [&]()
                                { HeaderBar::drawBatteryCluster(display, percent, charging); });
    }
    xSemaphoreGive(displayMutex);
}

// Separate entity from the header - only touches the panel when its own readings
// (current/voltage/free heap, at the precision they're actually displayed) change.
static void redrawFooter(bool force = false)
{
    float currentMa = fuelGauge.currentMilliAmps();
    float voltageMv = fuelGauge.voltageMilliVolts();
    uint32_t freeHeap = (uint32_t)dbgHeapFree();

    int16_t currentRounded = roundToInt16(currentMa);
    int16_t voltageCentiV = roundToInt16(voltageMv / 10.0f);
    int32_t heapKb = (int32_t)(freeHeap / 1024);

    bool statsChanged = force || currentRounded != lastFooterCurrentRounded || voltageCentiV != lastFooterVoltageCentiV;
    bool heapChanged = force || heapKb != lastFooterHeapKb;
    if (!statsChanged && !heapChanged)
        return; // nothing changed - skip the e-paper refresh entirely

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    lastFooterCurrentRounded = currentRounded;
    lastFooterVoltageCentiV = voltageCentiV;
    lastFooterHeapKb = heapKb;
    lastCurrentMa = currentMa; // used by drawHeaderAndContent()
    lastVoltageMv = voltageMv;

    if (dueForFullRefresh())
    {
        display.refreshFull(drawHeaderAndContent);
    }
    else
    {
        int16_t x, y, w, h;
        if (statsChanged)
        {
            FooterBar::statsRect(display, x, y, w, h);
            display.refreshPartial(x, y, w, h, [&]()
                                    { FooterBar::drawStats(display, currentMa, voltageMv); });
        }
        if (heapChanged)
        {
            FooterBar::heapRect(display, x, y, w, h);
            display.refreshPartial(x, y, w, h, [&]()
                                    { FooterBar::drawHeap(display, freeHeap); });
        }
    }
    xSemaphoreGive(displayMutex);
}

// Caller must hold displayMutex.
static void redrawFileBrowser()
{
    display.refreshFull(drawHeaderAndContent);
}

// Moves the selection by calling moveFn, then redraws just what changed - the
// cursor arrow, or the whole content area if the move scrolled the visible
// page - unless a periodic full refresh is due instead (see dueForFullRefresh()).
template <typename MoveFn>
static void moveSelectionAndRedraw(MoveFn moveFn)
{
    lastActivityMs = millis();
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    uint8_t previousSelected = fileBrowser.selectedIndex();
    moveFn();
    if (fileBrowser.selectedIndex() != previousSelected)
    {
        if (dueForFullRefresh())
            redrawFileBrowser();
        else
            fileBrowser.drawSelectionDelta(display, previousSelected);
    }
    xSemaphoreGive(displayMutex);
}

static void onPrevEdge()
{
    moveSelectionAndRedraw([]()
                            { fileBrowser.prev(); });
}

static void onNextEdge()
{
    moveSelectionAndRedraw([]()
                            { fileBrowser.next(); });
}

static void onActionEdge()
{
    lastActivityMs = millis();
    xSemaphoreTake(displayMutex, portMAX_DELAY);
    if (fileBrowser.activateSelected())
        redrawFileBrowser();
    xSemaphoreGive(displayMutex);
}

static void drawSleepIcon()
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

// Waking from SYSTEMOFF is a full reset - resumes at setup(), not here.
static void enterDeepSleep()
{
    Serial.println("No activity - entering deep sleep");
    Serial.flush();

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    display.refreshFull(drawSleepIcon);

    if (fuelGauge.present())
        saveLearnedParams(fuelGauge.learnedParams());

    digitalWrite(PIN_SD_ON, LOW);
    digitalWrite(PIN_SD_CS, HIGH);
    Wire.end();

    pinMode(PIN_BTN_PREV, INPUT_PULLUP_SENSE);
    pinMode(PIN_BTN_NEXT, INPUT_PULLUP_SENSE);
    pinMode(PIN_BTN_ACTION, INPUT_PULLUP_SENSE);

    delay(50); // let SPI/GPIO changes settle before cutting power
    NRF_POWER->SYSTEMOFF = POWER_SYSTEMOFF_SYSTEMOFF_Enter;
    while (true) {} // fallback - SYSTEMOFF should cut power before this ever runs
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

    InternalFS.begin();
    FuelGauge::LearnedParams savedParams;
    bool haveSavedParams = loadLearnedParams(savedParams);

    const FuelGauge::LearnedParams *seed = nullptr;
    if (haveSavedParams)
        seed = &savedParams; // this exact chip's own history - always wins
    else if (FACTORY_SEED_VALID)
        seed = &FACTORY_SEED; // never configured before - a real reference cell beats a blank guess

    if (!fuelGauge.begin(seed))
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

    // Prime all "last drawn" state from real readings before the first paint, so the
    // initial full refresh doesn't show stale sentinels for a moment.
    lastPercent = (uint8_t)fuelGauge.percent();
    lastCharging = fuelGauge.isCharging();
    lastCurrentMa = fuelGauge.currentMilliAmps();
    lastVoltageMv = fuelGauge.voltageMilliVolts();
    lastFooterCurrentRounded = roundToInt16(lastCurrentMa);
    lastFooterVoltageCentiV = roundToInt16(lastVoltageMv / 10.0f);
    lastFooterHeapKb = (int32_t)(dbgHeapFree() / 1024);

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    display.refreshFull(drawHeaderAndContent);
    xSemaphoreGive(displayMutex);

    lastActivityMs = millis();
}

void loop()
{
    static uint32_t lastFuelGaugePoll = 0;
    static uint32_t lastFuelGaugeSave = 0;
    delay(1000); // lets FreeRTOS tickless idle sleep instead of busy-polling

    uint32_t now = millis();
    if (now - lastFuelGaugePoll >= Config::FUEL_GAUGE_POLL_INTERVAL_MS)
    {
        lastFuelGaugePoll = now;
        redrawHeader();
        redrawFooter();
    }

    if (fuelGauge.present() && now - lastFuelGaugeSave >= Config::FUEL_GAUGE_SAVE_INTERVAL_MS)
    {
        lastFuelGaugeSave = now;
        saveLearnedParams(fuelGauge.learnedParams());
    }

    if (now - lastActivityMs >= Config::SLEEP_TIMEOUT_MS)
        enterDeepSleep();
}
