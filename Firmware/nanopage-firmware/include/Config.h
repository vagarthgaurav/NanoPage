// Tunable firmware-wide constants.

#pragma once

#include <cstdint>

namespace Config
{
    constexpr uint32_t FUEL_GAUGE_POLL_INTERVAL_MS = 1800000; // 30 minutes - fallback only, the ALRT interrupt drives normal updates
    constexpr uint16_t FULL_REFRESH_EVERY = 15; // partial e-paper updates ghost - clear the panel this often
    constexpr uint32_t SLEEP_TIMEOUT_MS = 1800000; // 30 minutes of no button activity
    constexpr uint32_t FUEL_GAUGE_SAVE_INTERVAL_MS = 3600000; // 1 hour - flash wear vs. freshness

    // Gates every Serial.print/println in main.cpp, plus the boot-time wait for a
    // terminal to connect. Flip to false for field/production builds.
    constexpr bool DEBUG_SERIAL = false;
} // namespace Config
