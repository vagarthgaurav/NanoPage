// Tunable firmware-wide constants.

#pragma once

#include <cstdint>

namespace Config
{
    constexpr uint32_t FUEL_GAUGE_POLL_INTERVAL_MS = 600000; // 10 minutes
    constexpr uint8_t FULL_REFRESH_EVERY = 8; // partial e-paper updates ghost - clear the panel this often
} // namespace Config
