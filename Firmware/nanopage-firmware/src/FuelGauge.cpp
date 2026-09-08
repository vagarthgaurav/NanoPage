#include "FuelGauge.h"

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

    uint16_t designCapReg = (uint16_t)(DESIGN_CAPACITY_MAH / CAPACITY_MAH_PER_LSB + 0.5f);
    float ichgTermMa = DESIGN_CAPACITY_MAH * 0.1f; // 0.1C termination current
    uint16_t ichgTermReg = (uint16_t)(ichgTermMa / CURRENT_MA_PER_LSB + 0.5f);
    uint16_t vEmptyReg = ((VEMPTY_MV / 10) << 7) | (VRECOVERY_MV / 40);

    // dQAcc/dPAcc scale FullCapNom to this pack's DesignCap - required by Maxim's
    // EZ Config sequence (UG6365 3.1) alongside DesignCap/IChgTerm/VEmpty, and
    // previously missing here, which left full-charge capacity tracking unset.
    uint16_t dqAccReg = designCapReg / 32;
    uint16_t dpAccReg = (uint16_t)(((uint32_t)dqAccReg * 44138UL) / designCapReg); // charge voltage <=4.275V

    writeReg(REG_DESIGNCAP, designCapReg);
    writeReg(REG_DQACC, dqAccReg);
    writeReg(REG_ICHGTERM, ichgTermReg);
    writeReg(REG_VEMPTY, vEmptyReg);
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
    writeReg(REG_STATUS, status & ~0x0002); // clear POR
}

// Reloads a previously-saved model snapshot instead of starting EZ Config from a
// blank slate. Sequence per Maxim's MAX17055 Software Implementation Guide (UG6365),
// sections 4.5-4.11 "Save learned parameters" / "Restoring capacity parameters".
void FuelGauge::restoreLearnedParams(const LearnedParams &saved)
{
    waitForPorRoutineDone();

    writeReg(REG_RCOMP0, saved.rcomp0);
    writeReg(REG_TEMPCO, saved.tempCo);
    writeReg(REG_FULLCAPNOM, saved.fullCapNom);
    delay(350);

    uint16_t mixSoc = 0;
    uint16_t fullCapNomNow = 0;
    readReg(REG_MIXSOC, mixSoc);
    readReg(REG_FULLCAPNOM, fullCapNomNow); // re-read - may have shifted slightly during the wait
    uint16_t mixCap = (uint16_t)(((uint32_t)mixSoc * fullCapNomNow) / 25600);
    writeReg(REG_MIXCAP, mixCap);

    writeReg(REG_FULLCAPREP, saved.fullCapRep);
    // dQAcc/dPAcc reload uses the originally-saved FullCapNom, not the re-read one.
    writeReg(REG_DPACC, 0x0C80);
    writeReg(REG_DQACC, saved.fullCapNom / 16);
    delay(350);

    writeReg(REG_CYCLES, saved.cycles);

    uint16_t status = 0;
    readReg(REG_STATUS, status);
    writeReg(REG_STATUS, status & ~0x0002); // clear POR
}

bool FuelGauge::begin(const LearnedParams *saved)
{
    wire_.beginTransmission(ADDR);
    present_ = wire_.endTransmission() == 0;
    if (!present_)
        return false;

    uint16_t status = 0;
    bool porSet = readReg(REG_STATUS, status) && (status & 0x0002);
    if (porSet && saved)
        restoreLearnedParams(*saved);
    else
        runEzConfigIfNeeded(); // no-op if POR flag is already clear

    return true;
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

bool FuelGauge::isCharging()
{
    return currentMilliAmps() > CHARGING_DEADBAND_MA;
}
