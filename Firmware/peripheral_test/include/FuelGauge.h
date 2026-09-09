// MAX17055 fuel gauge (I2C 0x36). Register access ported from peripheral_test/src/main.cpp.

#pragma once

#include <Arduino.h>
#include <Wire.h>

class FuelGauge
{
public:
    explicit FuelGauge(TwoWire &wire = Wire) : wire_(wire) {}

    // What the chip has learned about this specific cell so far - lives only in its
    // registers and is forgotten if it ever loses power (e.g. battery unplugged).
    // Plain aggregate (no default member initializers) so it stays constexpr-brace-
    // initializable in the C++ dialect this toolchain builds with.
    struct LearnedParams
    {
        uint16_t rcomp0;
        uint16_t tempCo;
        uint16_t fullCapNom;
        uint16_t fullCapRep;
        uint16_t cycles;
    };

    // If the chip's POR flag is set (lost power) and `saved` is non-null, reloads
    // those learned parameters instead of starting EZ Config from a blank slate.
    bool begin(const LearnedParams *saved = nullptr);

    bool present() const { return present_; }

    float percent(); // state of charge, 0..100
    float voltageMilliVolts();
    float currentMilliAmps();
    bool isCharging(); // net current above a small deadband

    // Snapshot the model's current learned state, for persisting outside the chip.
    LearnedParams learnedParams();

    // One-shot Serial dump of the EZ-Config/model registers, for comparing against
    // the datasheet's expected values or across burn-test cycles.
    void debugDumpRegisters();

    // Board has a generic 2-pin JST connector - must match whatever pack is fitted.
    static constexpr float DESIGN_CAPACITY_MAH = 320.0f;
    static constexpr uint16_t VEMPTY_MV = 3300;
    static constexpr uint16_t VRECOVERY_MV = 3880;

private:
    static constexpr uint8_t ADDR = 0x36;
    static constexpr uint8_t REG_STATUS = 0x00;    // bit1 = POR
    static constexpr uint8_t REG_REPSOC = 0x06;    // 1/256 %/LSB
    static constexpr uint8_t REG_VCELL = 0x09;     // 78.125uV/LSB
    static constexpr uint8_t REG_CURRENT = 0x0A;   // signed, 1.5625uV/RSENSE per LSB
    static constexpr uint8_t REG_MIXSOC = 0x0D;     // used only to recompute MixCap on restore
    static constexpr uint8_t REG_MIXCAP = 0x0F;     // same LSB as DesignCap
    static constexpr uint8_t REG_FULLCAPREP = 0x10; // same LSB as DesignCap
    static constexpr uint8_t REG_CYCLES = 0x17;     // 1%/LSB of a full cycle
    static constexpr uint8_t REG_DESIGNCAP = 0x18;  // 5.0uVh/RSENSE per LSB
    static constexpr uint8_t REG_FULLCAPNOM = 0x23; // same LSB as DesignCap
    static constexpr uint8_t REG_ICHGTERM = 0x1E; // same LSB as Current
    static constexpr uint8_t REG_RCOMP0 = 0x38;
    static constexpr uint8_t REG_TEMPCO = 0x39;
    static constexpr uint8_t REG_VEMPTY = 0x3A;
    static constexpr uint8_t REG_MODELCFG = 0xDB;
    static constexpr uint8_t REG_FSTAT = 0x3D; // bit0 = DNR
    static constexpr uint8_t REG_DQACC = 0x45;
    static constexpr uint8_t REG_DPACC = 0x46;
    static constexpr uint8_t REG_HIBCFG = 0xBA;
    static constexpr uint8_t REG_COMMAND = 0x60;

    static constexpr float RSENSE_OHMS = 0.010f; // current-sense resistor on CSP/CSN
    static constexpr float CURRENT_MA_PER_LSB = 1.5625f / RSENSE_OHMS / 1000.0f;
    static constexpr float CAPACITY_MAH_PER_LSB = 5.0f / (RSENSE_OHMS * 1000.0f);
    static constexpr float CHARGING_DEADBAND_MA = 2.0f;

    bool readReg(uint8_t reg, uint16_t &out);
    bool writeReg(uint8_t reg, uint16_t value);
    // Writes DesignCap/IChgTerm/VEmpty from this board's fixed pack spec (not
    // learned per-chip state) - called unconditionally from begin() since
    // restoreLearnedParams() deliberately doesn't touch these three.
    void writeCapacityConfig();
    void runEzConfigIfNeeded();
    void restoreLearnedParams(const LearnedParams &saved);
    void waitForPorRoutineDone(); // FSTAT.DNR - shared by EZ Config and restore

    TwoWire &wire_;
    bool present_ = false;
};
