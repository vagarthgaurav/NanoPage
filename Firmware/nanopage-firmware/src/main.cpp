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
    // flash lets a real POR reload this exact chip/battery's own history instead
    // of falling back to FACTORY_SEED every time.
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

    // False for a genuinely new/never-saved board, a corrupted read, or a snapshot
    // that fails FuelGauge::learnedParamsPlausible() - e.g. this session's ~4x
    // phantom-capacity bug, so a bad save can't get faithfully restored forever.
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
        if (!FuelGauge::learnedParamsPlausible(stored.params))
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
    constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x003E, 0x223E, 0x02B2, 0x0277, 0x0031};
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

// Shared with the fuel-gauge ALRT handler below, which resets this on a serviced
// alert so the fallback poll timer counts from the last real update either way.
static uint32_t lastFuelGaugePoll = 0;

static volatile uint32_t lastActivityMs = 0;

static int16_t roundToInt16(float v)
{
    return (int16_t)(v >= 0 ? v + 0.5f : v - 0.5f);
}

// BQ25170 STAT, not the fuel gauge's current reading: charge current ramps up/down
// over a couple of seconds around plug/unplug, so a current-threshold test lags the
// physical event by a few seconds in both directions. STAT reflects the charger's
// own state and flips within its internal debounce time.
static bool chargerActive()
{
    return digitalRead(PIN_CHG_STAT) == LOW;
}

// Diagnostic only (not wired into the UI yet) - distinguishes "no real input power"
// (cable/port issue) from "charger genuinely terminated" while charging=false.
static bool inputPowerGood()
{
    return digitalRead(PIN_CHG_PG) == LOW;
}

// Shared across every narrow partial-refresh call site below (header, footer,
// cursor move) - fast partial updates ghost over time, so this forces a full
// clear periodically regardless of which region asks. Every candidate partial
// refresh counts, whichever region it's for.
static bool dueForFullRefresh()
{
    bool due = ++partialRefreshCount % Config::FULL_REFRESH_EVERY == 0;
    if (due && Config::DEBUG_SERIAL)
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
    uint8_t percent = (uint8_t)(fuelGauge.percent() + 0.5f);
    bool charging = chargerActive();

    if (!force && percent == lastPercent && charging == lastCharging)
        return; // nothing changed - skip the e-paper refresh entirely

    xSemaphoreTake(displayMutex, portMAX_DELAY);
    lastPercent = percent;
    lastCharging = charging;

    if (Config::DEBUG_SERIAL)
    {
        Serial.print("Fuel gauge: ");
        Serial.print(percent);
        Serial.print("%, charging=");
        Serial.print(charging ? "true" : "false");
        Serial.print(", powerGood=");
        Serial.print(inputPowerGood() ? "true" : "false");
        Serial.print(", current=");
        Serial.print(fuelGauge.currentMilliAmps());
        Serial.println("mA");
    }

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

// MAX17055 ALRT - fires on every ~1% RepSOC change (see FuelGauge::enableSocChangeAlert).
// Open-drain/active-low, so ALRT stays asserted until the status flag is cleared.
static void onFuelGaugeAlertEdge()
{
    fuelGauge.clearSocChangeAlert();
    lastFuelGaugePoll = millis(); // fallback poll now counts from this real update
    redrawHeader();
    redrawFooter();
}

// BQ25170 STAT - fires the instant charging starts or stops (USB plugged in,
// unplugged, or charge complete), independent of the fuel gauge's own current
// reading (see chargerActive()).
static void onChargerStatusChangeEdge()
{
    redrawHeader();
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
    if (Config::DEBUG_SERIAL)
    {
        Serial.println("No activity - entering deep sleep");
        Serial.flush();
    }

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
    while (true)
    {
    } // fallback - SYSTEMOFF should cut power before this ever runs
}

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

    displayMutex = xSemaphoreCreateMutex();

    Wire.begin();
    Wire.setClock(100000);

    InternalFS.begin();
    FuelGauge::LearnedParams savedParams;
    bool haveSavedParams = loadLearnedParams(savedParams);

    // begin() only actually restores this seed when the chip's own POR flag is set
    // - i.e. a genuinely new/never-configured chip, or a real battery disconnect/
    // reconnect. Otherwise it's a no-op and the chip's live registers (already
    // tracking this specific cell's history) are left alone. When it does apply:
    // this exact chip/battery's own flash-saved history wins if we have it (a real
    // reference beats a guess), otherwise FACTORY_SEED (a genuinely new board).
    const FuelGauge::LearnedParams *seed = haveSavedParams ? &savedParams : &FACTORY_SEED;

    if (!fuelGauge.begin(seed) && Config::DEBUG_SERIAL)
        Serial.println("Fuel gauge: not found at 0x36");
    else if (Config::DEBUG_SERIAL)
    {
        Serial.print("Fuel gauge: seed=");
        Serial.println(haveSavedParams ? "flash" : "factory");
        fuelGauge.debugDumpRegisters();
    }

    // One-shot, manually-triggered wipe: discard this chip's learned history and
    // force a fresh restore from FACTORY_SEED, e.g. after swapping in a new
    // battery. Flip Config::FORCE_FUELGAUGE_RECAL back to false and reflash once
    // confirmed. Deletes the stale flash snapshot so it isn't reloaded next boot -
    // note the caveat on forceFullReinit() below still applies here: a
    // software-faked POR doesn't fully reset the chip's internal MixSOC
    // accumulator, so a real battery disconnect/reconnect afterward is still the
    // most reliable way to get a fully clean state (see
    // project_fuelgauge_ez_config_calibration memory).
    if (Config::FORCE_FUELGAUGE_RECAL && fuelGauge.present())
    {
        fuelGauge.forceFullReinit(&FACTORY_SEED);
        InternalFS.remove(LEARNED_PARAMS_PATH);
        if (Config::DEBUG_SERIAL)
        {
            Serial.println("Fuel gauge: forced restore from factory reference");
            fuelGauge.debugDumpRegisters();
        }
    }

    pinMode(PIN_BTN_PREV, INPUT_PULLUP);
    pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
    pinMode(PIN_BTN_ACTION, INPUT_PULLUP);
    attachInterrupt(PIN_BTN_PREV, onPrevEdge, FALLING | ISR_DEFERRED);
    attachInterrupt(PIN_BTN_NEXT, onNextEdge, FALLING | ISR_DEFERRED);
    attachInterrupt(PIN_BTN_ACTION, onActionEdge, FALLING | ISR_DEFERRED);

    pinMode(PIN_FUEL_ALERT, INPUT_PULLUP); // open-drain, active low
    attachInterrupt(PIN_FUEL_ALERT, onFuelGaugeAlertEdge, FALLING | ISR_DEFERRED);

    pinMode(PIN_CHG_STAT, INPUT_PULLUP); // open-drain - belt-and-suspenders, matches PIN_FUEL_ALERT below
    attachInterrupt(PIN_CHG_STAT, onChargerStatusChangeEdge, CHANGE | ISR_DEFERRED);

    pinMode(PIN_CHG_PG, INPUT_PULLUP); // open-drain, diagnostic only for now (see inputPowerGood())

    display.begin();

    // Only FileBrowser is wired up so far - see PageMode.h.
    if (!fileBrowser.begin() && Config::DEBUG_SERIAL)
        Serial.println("SD card: not found");

    // Prime all "last drawn" state from real readings before the first paint, so the
    // initial full refresh doesn't show stale sentinels for a moment.
    lastPercent = (uint8_t)(fuelGauge.percent() + 0.5f);
    lastCharging = chargerActive();
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
