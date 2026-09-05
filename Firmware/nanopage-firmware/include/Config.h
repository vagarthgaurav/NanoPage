// Tunable firmware-wide constants.

#pragma once

#include <cstdint>

namespace Config
{
    constexpr uint32_t FUEL_GAUGE_POLL_INTERVAL_MS = 600000; // 10 minutes
    constexpr uint8_t FULL_REFRESH_EVERY = 8; // partial e-paper updates ghost - clear the panel this often
    constexpr uint32_t SLEEP_TIMEOUT_MS = 1800000; // 30 minutes of no button activity
} // namespace Config
