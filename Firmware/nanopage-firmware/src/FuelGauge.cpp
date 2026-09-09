#include "FuelGauge.h"
#include "Config.h"

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
