#include "FuelGauge.h"
#include "Config.h"

#include <InternalFileSystem.h>

namespace
{
    // The fuel gauge's learned model lives only in its own registers and is lost
    // if the battery is unplugged - mirrored to flash so a POR can reload it.
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
    bool loadLearnedParamsFile(FuelGauge::LearnedParams &out)
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

    void saveLearnedParamsFile(const FuelGauge::LearnedParams &p)
    {
        using Adafruit_LittleFS_Namespace::FILE_O_WRITE;
        StoredLearnedParams stored{p, checksumOf(p)};
        Adafruit_LittleFS_Namespace::File f = InternalFS.open(LEARNED_PARAMS_PATH, FILE_O_WRITE);
        if (!f)
            return;
        f.write(reinterpret_cast<const uint8_t *>(&stored), sizeof(stored));
        f.close();
    }

    // Reference snapshot from a calibrated reference board (same EEMB LP402535
    // pack) - see README.md "Fuel gauge factory seed" for provenance.
    // Order: rcomp0, tempCo, fullCapNom, fullCapRep, cycles.
    constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x003E, 0x223E, 0x02B2, 0x0277, 0x0031};

    // attachInterrupt() needs a plain function pointer, so the single fuel-gauge
    // instance and its caller-supplied callback are stashed here for the trampoline
    // below rather than passed as a capturing lambda.
    FuelGauge *g_alertInstance = nullptr;
    void (*g_onAlert)() = nullptr;

    void onFuelAlertEdge()
    {
        if (g_alertInstance)
            g_alertInstance->clearSocChangeAlert();
        if (g_onAlert)
            g_onAlert();
    }
} // namespace

bool FuelGauge::readReg(uint8_t reg, uint16_t &out)
{
    wire_.beginTransmission(ADDR);
    wire_.write(reg);
    if (wire_.endTransmission(false) != 0)
        return false;
    if (wire_.requestFrom(ADDR, (uint8_t)2) != 2)
        return false;
    uint8_t lo = wire_.read();
    uint8_t hi = wire_.read();
    out = (uint16_t)lo | ((uint16_t)hi << 8); // LSB first
    return true;
}

bool FuelGauge::writeReg(uint8_t reg, uint16_t value)
{
    wire_.beginTransmission(ADDR);
    wire_.write(reg);
    wire_.write((uint8_t)(value & 0xFF));
    wire_.write((uint8_t)(value >> 8));
    return wire_.endTransmission() == 0;
}

bool FuelGauge::writeAndVerifyReg(uint8_t reg, uint16_t value)
{
    for (int attempt = 0; attempt < 3; attempt++)
    {
        writeReg(reg, value);
        delay(1);
        uint16_t readBack = 0;
        if (readReg(reg, readBack) && readBack == value)
            return true;
    }
    return false;
}

void FuelGauge::waitForPorRoutineDone()
{
    uint16_t fstat = 0;
    uint32_t waitStart = millis();
    do
    {
        readReg(REG_FSTAT, fstat);
        if (!(fstat & 0x0001))
            break;
        delay(10);
    } while (millis() - waitStart < 1000);
}

void FuelGauge::writeCapacityConfig()
{
    uint16_t designCapReg = (uint16_t)(DESIGN_CAPACITY_MAH / CAPACITY_MAH_PER_LSB + 0.5f);
    float ichgTermMa = DESIGN_CAPACITY_MAH * 0.1f; // 0.1C termination current
    uint16_t ichgTermReg = (uint16_t)(ichgTermMa / CURRENT_MA_PER_LSB + 0.5f);
    uint16_t vEmptyReg = ((VEMPTY_MV / 10) << 7) | (VRECOVERY_MV / 40);

    writeReg(REG_DESIGNCAP, designCapReg);
    writeReg(REG_ICHGTERM, ichgTermReg);
    writeReg(REG_VEMPTY, vEmptyReg);
}

// MAX17055 "EZ Config" init (no OCV table), Maxim's documented POR sequence.
void FuelGauge::runEzConfigIfNeeded()
{
    uint16_t status = 0;
    if (!readReg(REG_STATUS, status))
        return;
    if (!(status & 0x0002))
        return; // POR flag clear, already configured

    waitForPorRoutineDone();

    uint16_t hibCfg = 0;
    readReg(REG_HIBCFG, hibCfg);
    writeReg(REG_COMMAND, 0x90); // exit hibernate, step 1
    writeReg(REG_HIBCFG, 0x0);
    writeReg(REG_COMMAND, 0x0); // exit hibernate, step 3

    // DesignCap/IChgTerm/VEmpty already (re)written unconditionally in begin().
    uint16_t designCapReg = (uint16_t)(DESIGN_CAPACITY_MAH / CAPACITY_MAH_PER_LSB + 0.5f);

    // dQAcc/dPAcc scale FullCapNom to this pack's DesignCap - required by Maxim's
    // EZ Config sequence (UG6365 3.1) alongside DesignCap/IChgTerm/VEmpty, and
    // previously missing here, which left full-charge capacity tracking unset.
    uint16_t dqAccReg = designCapReg / 32;
    uint16_t dpAccReg = (uint16_t)(((uint32_t)dqAccReg * 44138UL) / designCapReg); // charge voltage <=4.275V

    writeReg(REG_DQACC, dqAccReg);
    writeReg(REG_DPACC, dpAccReg);
    writeReg(REG_MODELCFG, 0x8000); // Refresh=1, default Li-ion model

    uint16_t modelCfg = 0x8000;
    uint32_t waitStart = millis();
    do
    {
        readReg(REG_MODELCFG, modelCfg);
        if (!(modelCfg & 0x8000))
            break;
        delay(10);
    } while (millis() - waitStart < 1000);

    writeReg(REG_HIBCFG, hibCfg);

    readReg(REG_STATUS, status);
    writeAndVerifyReg(REG_STATUS, status & ~0x0002); // clear POR
}

// Reloads a previously-saved model snapshot instead of starting EZ Config from a
// blank slate. Sequence per Maxim's MAX17055 Software Implementation Guide (UG6365),
// sections 4.5-4.11 "Save learned parameters" / "Restoring capacity parameters".
void FuelGauge::restoreLearnedParams(const LearnedParams &saved)
{
    waitForPorRoutineDone();

    writeAndVerifyReg(REG_RCOMP0, saved.rcomp0);
    writeAndVerifyReg(REG_TEMPCO, saved.tempCo);
    writeAndVerifyReg(REG_FULLCAPNOM, saved.fullCapNom);
    delay(350);

    uint16_t mixSoc = 0;
    uint16_t fullCapNomNow = 0;
    readReg(REG_MIXSOC, mixSoc);
    readReg(REG_FULLCAPNOM, fullCapNomNow); // re-read - may have shifted slightly during the wait
    // Diagnostic: is MixSOC itself already corrupted at this point (before this
    // restore has written anything), or does corruption creep in afterward? See
    // project_fuelgauge_ez_config_calibration memory.
    if (Config::DEBUG_SERIAL)
    {
        Serial.print("Fuel gauge: raw post-POR MixSOC before restore = 0x");
        Serial.print(mixSoc, HEX);
        Serial.print(" (");
        Serial.print(mixSoc / 256.0f);
        Serial.println("%)");
    }
    uint16_t mixCap = (uint16_t)(((uint32_t)mixSoc * fullCapNomNow) / 25600);
    // MixCap/FullCapRep are ModelGauge algorithm outputs the chip keeps adjusting on
    // its own (UG6365) - a plain write can lose a race against the chip's own write
    // and get silently dropped, so these (and everything else in this restore
    // sequence) must be write-verified per the datasheet's documented procedure.
    writeAndVerifyReg(REG_MIXCAP, mixCap);

    writeAndVerifyReg(REG_FULLCAPREP, saved.fullCapRep);
    // dQAcc/dPAcc reload uses the originally-saved FullCapNom, not the re-read one.
    writeAndVerifyReg(REG_DPACC, 0x0C80);
    writeAndVerifyReg(REG_DQACC, saved.fullCapNom / 16);
    delay(350);

    writeAndVerifyReg(REG_CYCLES, saved.cycles);

    uint16_t status = 0;
    readReg(REG_STATUS, status);
    writeAndVerifyReg(REG_STATUS, status & ~0x0002); // clear POR
}

bool FuelGauge::begin(const LearnedParams *saved)
{
    wire_.beginTransmission(ADDR);
    present_ = wire_.endTransmission() == 0;
    if (!present_)
        return false;

    uint16_t status = 0;
    bool porSet = readReg(REG_STATUS, status) && (status & 0x0002);

    // DesignCap/IChgTerm/VEmpty reflect this board's fixed pack spec, not learned
    // per-chip history - keep them correct on every boot regardless of which path
    // below runs. restoreLearnedParams() deliberately skips them (Maxim's restore
    // procedure only covers the learned model params), so without this they can be
    // left stale indefinitely - e.g. from an earlier firmware revision's EZ Config,
    // or the chip's own POR defaults if a real power-loss ever reset them and the
    // saved-seed restore path was taken afterward instead of a fresh EZ Config.
    writeCapacityConfig();

    // Per UG6365, a full EZ-Config model load (ModelCfg.Refresh + friends) runs on
    // every real POR unconditionally - that's what makes the chip's own hardware
    // (re)compute a valid MixSOC/VFSOC from the current cell voltage. Restoring
    // previously-learned params (RCOMP0/TempCo/FullCapNom/FullCapRep/Cycles) is
    // documented as an *overlay* on top of that, not a substitute for it. Treating
    // them as mutually exclusive (skipping the model load whenever a saved seed
    // was available) left MixSOC pinned at its POR default (observed as 0xFFFF/
    // 256%, an obviously invalid value) instead of ever being properly
    // reinitialized - see project_fuelgauge_ez_config_calibration memory.
    runEzConfigIfNeeded(); // no-op if POR flag is already clear

    if (porSet && saved)
        restoreLearnedParams(*saved);

    enableSocChangeAlert();

    return true;
}

bool FuelGauge::beginWithSavedState()
{
    InternalFS.begin();
    LearnedParams savedParams;
    bool haveSavedParams = loadLearnedParamsFile(savedParams);

    // begin() only actually restores this seed when the chip's own POR flag is set
    // - i.e. a genuinely new/never-configured chip, or a real battery disconnect/
    // reconnect. Otherwise it's a no-op and the chip's live registers (already
    // tracking this specific cell's history) are left alone. When it does apply:
    // this exact chip/battery's own flash-saved history wins if we have it (a real
    // reference beats a guess), otherwise FACTORY_SEED (a genuinely new board).
    const LearnedParams *seed = haveSavedParams ? &savedParams : &FACTORY_SEED;

    if (!begin(seed) && Config::DEBUG_SERIAL)
        Serial.println("Fuel gauge: not found at 0x36");
    else if (Config::DEBUG_SERIAL)
    {
        Serial.print("Fuel gauge: seed=");
        Serial.println(haveSavedParams ? "flash" : "factory");
        debugDumpRegisters();
    }

    // One-shot, manually-triggered wipe: discard this chip's learned history and
    // force a fresh restore from FACTORY_SEED, e.g. after swapping in a new
    // battery. Flip Config::FORCE_FUELGAUGE_RECAL back to false and reflash once
    // confirmed. Deletes the stale flash snapshot so it isn't reloaded next boot -
    // note the caveat on forceFullReinit() above still applies here: a
    // software-faked POR doesn't fully reset the chip's internal MixSOC
    // accumulator, so a real battery disconnect/reconnect afterward is still the
    // most reliable way to get a fully clean state (see
    // project_fuelgauge_ez_config_calibration memory).
    if (Config::FORCE_FUELGAUGE_RECAL && present_)
    {
        forceFullReinit(&FACTORY_SEED);
        InternalFS.remove(LEARNED_PARAMS_PATH);
        if (Config::DEBUG_SERIAL)
        {
            Serial.println("Fuel gauge: forced restore from factory reference");
            debugDumpRegisters();
        }
    }

    return present_;
}

void FuelGauge::saveLearnedParamsToFlash()
{
    if (!present_)
        return;
    saveLearnedParamsFile(learnedParams());
}

// Lets ALRT pulse low on every ~1% RepSOC step, so the caller can redraw on that
// edge instead of only on its own poll timer. Read-modify-write so it doesn't
// disturb other CONFIG/CONFIG2 bits (defaults, or ones a future feature sets).
void FuelGauge::enableSocChangeAlert()
{
    uint16_t config = 0;
    readReg(REG_CONFIG, config);
    writeReg(REG_CONFIG, config | CONFIG_AEN);

    uint16_t config2 = 0;
    readReg(REG_CONFIG2, config2);
    writeReg(REG_CONFIG2, config2 | CONFIG2_DSOCEN);
}

void FuelGauge::clearSocChangeAlert()
{
    uint16_t status = 0;
    readReg(REG_STATUS, status);
    writeReg(REG_STATUS, status & ~STATUS_DSOCI);
}

void FuelGauge::enableAlertInterrupt(void (*onAlert)())
{
    g_alertInstance = this;
    g_onAlert = onAlert;

    pinMode(PIN_FUEL_ALERT, INPUT_PULLUP); // open-drain, active low
    attachInterrupt(PIN_FUEL_ALERT, onFuelAlertEdge, FALLING | ISR_DEFERRED);

    // FALLING only fires on a fresh edge - if dSOCi (or another Aen source) was
    // already asserted from before this boot (the chip stays powered through an
    // MCU reset/sleep, unlike this interrupt registration), the pin is already
    // low and no edge will ever come. Without this, the alert-driven display
    // update silently stops firing until the next full percent swings back up
    // past a boundary, or the 30-minute fallback poll (which also never clears
    // the flag) papers over it.
    if (digitalRead(PIN_FUEL_ALERT) == LOW)
        onFuelAlertEdge();
}

FuelGauge::LearnedParams FuelGauge::learnedParams()
{
    LearnedParams p;
    readReg(REG_RCOMP0, p.rcomp0);
    readReg(REG_TEMPCO, p.tempCo);
    readReg(REG_FULLCAPNOM, p.fullCapNom);
    readReg(REG_FULLCAPREP, p.fullCapRep);
    readReg(REG_CYCLES, p.cycles);
    return p;
}

void FuelGauge::forceFullReinit(const LearnedParams *ref)
{
    uint16_t status = 0;
    readReg(REG_STATUS, status);
    writeReg(REG_STATUS, status | 0x0002); // fake a POR so runEzConfigIfNeeded() proceeds
    runEzConfigIfNeeded();
    if (ref)
        restoreLearnedParams(*ref);
}

bool FuelGauge::learnedParamsPlausible(const LearnedParams &p)
{
    auto capacityInRangeMah = [](uint16_t raw)
    {
        float mah = raw * CAPACITY_MAH_PER_LSB;
        return mah > DESIGN_CAPACITY_MAH * 0.5f && mah < DESIGN_CAPACITY_MAH * 2.0f;
    };
    return capacityInRangeMah(p.fullCapRep) && capacityInRangeMah(p.fullCapNom);
}

void FuelGauge::logStatus(bool charging, bool powerGood)
{
    if (!Config::DEBUG_SERIAL)
        return;

    Serial.print("Fuel gauge: ");
    Serial.print((uint8_t)(percent() + 0.5f));
    Serial.print("%, charging=");
    Serial.print(charging ? "true" : "false");
    Serial.print(", powerGood=");
    Serial.print(powerGood ? "true" : "false");
    Serial.print(", current=");
    Serial.print(currentMilliAmps());
    Serial.print("mA, voltage=");
    Serial.print(voltageMilliVolts());
    Serial.println("mV");
}

void FuelGauge::debugDumpRegisters()
{
    struct NamedReg
    {
        const char *name;
        uint8_t reg;
    };
    static const NamedReg regs[] = {
        {"DesignCap", REG_DESIGNCAP},
        {"IChgTerm", REG_ICHGTERM},
        {"VEmpty", REG_VEMPTY},
        {"dQAcc", REG_DQACC},
        {"dPAcc", REG_DPACC},
        {"RCOMP0", REG_RCOMP0},
        {"TempCo", REG_TEMPCO},
        {"FullCapNom", REG_FULLCAPNOM},
        {"FullCapRep", REG_FULLCAPREP},
        {"Cycles", REG_CYCLES},
        {"MixSOC", REG_MIXSOC},
        {"MixCap", REG_MIXCAP},
        {"RepSOC", REG_REPSOC},
        {"VCell", REG_VCELL},
        {"Current", REG_CURRENT},
        {"FSTAT", REG_FSTAT},
        {"Status", REG_STATUS},
    };
    Serial.println("Fuel gauge register dump:");
    for (const auto &r : regs)
    {
        uint16_t value = 0;
        readReg(r.reg, value);
        Serial.print("  ");
        Serial.print(r.name);
        Serial.print(" (0x");
        Serial.print(r.reg, HEX);
        Serial.print(") = 0x");
        Serial.print(value, HEX);
        Serial.print("  -> ");
        Serial.println(decodeRegister(r.reg, value));
    }
}

String FuelGauge::decodeRegister(uint8_t reg, uint16_t raw)
{
    char buf[48];
    switch (reg)
    {
    case REG_DESIGNCAP:
    case REG_FULLCAPNOM:
    case REG_FULLCAPREP:
    case REG_MIXCAP:
    case REG_DQACC:
        snprintf(buf, sizeof(buf), "%.1f mAh", raw * CAPACITY_MAH_PER_LSB);
        return String(buf);
    case REG_ICHGTERM:
        snprintf(buf, sizeof(buf), "%.1f mA", raw * CURRENT_MA_PER_LSB);
        return String(buf);
    case REG_CURRENT:
        snprintf(buf, sizeof(buf), "%.1f mA", (int16_t)raw * CURRENT_MA_PER_LSB);
        return String(buf);
    case REG_VEMPTY:
        snprintf(buf, sizeof(buf), "VEmpty=%umV VRecovery=%umV", ((raw >> 7) & 0x1FF) * 10, (raw & 0x7F) * 40);
        return String(buf);
    case REG_VCELL:
        snprintf(buf, sizeof(buf), "%.3f V", raw * 78.125f / 1000000.0f);
        return String(buf);
    case REG_DPACC:
        snprintf(buf, sizeof(buf), "%.2f %%", raw * 0.0625f);
        return String(buf);
    case REG_MIXSOC:
    case REG_REPSOC:
        snprintf(buf, sizeof(buf), "%.2f %%", raw / 256.0f);
        return String(buf);
    case REG_CYCLES:
        snprintf(buf, sizeof(buf), "%.2f cycles", raw / 100.0f);
        return String(buf);
    case REG_FSTAT:
        return raw & 0x0001 ? "DNR=1 (not ready)" : "DNR=0 (ready)";
    case REG_STATUS:
        snprintf(buf, sizeof(buf), "POR=%d dSOCi=%d", (raw >> 1) & 1, (raw >> 7) & 1);
        return String(buf);
    case REG_RCOMP0:
    case REG_TEMPCO:
        snprintf(buf, sizeof(buf), "%u (calibration coefficient, no direct unit)", raw);
        return String(buf);
    default:
        return String(raw);
    }
}

float FuelGauge::percent()
{
    uint16_t raw = 0;
    if (!readReg(REG_REPSOC, raw))
        return 0.0f;
    return raw / 256.0f;
}

float FuelGauge::voltageMilliVolts()
{
    uint16_t raw = 0;
    if (!readReg(REG_VCELL, raw))
        return 0.0f;
    return raw * 78.125f / 1000.0f;
}

float FuelGauge::currentMilliAmps()
{
    uint16_t raw = 0;
    if (!readReg(REG_CURRENT, raw))
        return 0.0f;
    return (int16_t)raw * CURRENT_MA_PER_LSB;
}
