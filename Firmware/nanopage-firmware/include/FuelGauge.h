// MAX17055 fuel gauge (I2C 0x36). Register access ported from peripheral_test/src/main.cpp.

#pragma once

#include <Arduino.h>
#include <Wire.h>

class FuelGauge
{
public:
    explicit FuelGauge(TwoWire &wire = Wire) : wire_(wire) {}

    // Also runs the one-time EZ Config sequence if the chip's POR flag is set.
    bool begin();

    bool present() const { return present_; }

    float percent(); // state of charge, 0..100
    float voltageMilliVolts();
    float currentMilliAmps();
    bool isCharging(); // net current above a small deadband

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
    static constexpr uint8_t REG_DESIGNCAP = 0x18; // 5.0uVh/RSENSE per LSB
    static constexpr uint8_t REG_ICHGTERM = 0x1E; // same LSB as Current
    static constexpr uint8_t REG_DQACC = 0x45;
    static constexpr uint8_t REG_DPACC = 0x46;
    static constexpr uint8_t REG_VEMPTY = 0x3A;
    static constexpr uint8_t REG_MODELCFG = 0xDB;
    static constexpr uint8_t REG_FSTAT = 0x3D; // bit0 = DNR
    static constexpr uint8_t REG_HIBCFG = 0xBA;
    static constexpr uint8_t REG_COMMAND = 0x60;

    static constexpr float RSENSE_OHMS = 0.010f; // current-sense resistor on CSP/CSN
    static constexpr float CURRENT_MA_PER_LSB = 1.5625f / RSENSE_OHMS / 1000.0f;
    static constexpr float CAPACITY_MAH_PER_LSB = 5.0f / (RSENSE_OHMS * 1000.0f);
    static constexpr float CHARGING_DEADBAND_MA = 2.0f;

    bool readReg(uint8_t reg, uint16_t &out);
    bool writeReg(uint8_t reg, uint16_t value);
    void runEzConfigIfNeeded();

    TwoWire &wire_;
    bool present_ = false;
};
