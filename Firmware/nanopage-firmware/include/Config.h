// Tunable firmware-wide constants.

#pragma once

#include <cstdint>

namespace Config
{
    constexpr uint32_t FUEL_GAUGE_POLL_INTERVAL_MS = 1800000; // 30 minutes - fallback only, the ALRT interrupt drives normal updates
    constexpr uint16_t FULL_REFRESH_EVERY = 10; // partial e-paper updates ghost - clear the panel this often
    constexpr uint32_t SLEEP_TIMEOUT_MS = 1800000; // 30 minutes of no button activity
    constexpr uint32_t FUEL_GAUGE_SAVE_INTERVAL_MS = 3600000; // 1 hour - flash wear vs. freshness

    // Gates every Serial.print/println in main.cpp, plus the boot-time wait for a
    // terminal to connect. Flip to false for field/production builds.
    constexpr bool DEBUG_SERIAL = false;

    // One-shot: forces the fuel gauge to discard its corrupted learned FullCapRep
    // and restore from FACTORY_SEED instead, deleting the stale flash snapshot so
    // it isn't reloaded later. Flip back to false and reflash once confirmed - a
    // software-faked POR doesn't reset the chip's internal MixSOC accumulator, so
    // this alone isn't sufficient; a real battery disconnect/reconnect is still
    // needed to fully recover (see project_fuelgauge_ez_config_calibration memory).
    constexpr bool FORCE_FUELGAUGE_RECAL = false;
} // namespace Config
