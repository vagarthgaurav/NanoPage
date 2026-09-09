// Tunable firmware-wide constants.

#pragma once

#include <cstdint>

namespace Config
{
    constexpr uint32_t FUEL_GAUGE_POLL_INTERVAL_MS = 1800000; // 30 minutes - fallback only, the ALRT interrupt drives normal updates
    constexpr uint32_t FOOTER_UPDATE_INTERVAL_MS = 60000; // 1 minute - footer's voltage/current are debug data, not event-driven like the header's SoC
    constexpr uint16_t FULL_REFRESH_EVERY = 25; // partial e-paper updates ghost - clear the panel this often
    constexpr uint32_t SLEEP_TIMEOUT_MS = 1800000; // 30 minutes of no button activity
    constexpr uint32_t FUEL_GAUGE_SAVE_INTERVAL_MS = 3600000; // 1 hour - flash wear vs. freshness

    // Gates every Serial.print/println in main.cpp, including Serial.begin()
    // itself and the boot-time wait for a terminal. Flip to false for
    // field/production builds.
    //
    // Confirmed on hardware (2026-09-09): unconditionally calling
    // Serial.begin() while leaving the app's own prints gated behind this flag
    // did NOT fix the display corruption in Display.h - it garbled the same
    // way with DEBUG_SERIAL=false whether or not a host was actively reading
    // the port. The actual print traffic (not USB enumeration, not an active
    // reader) is what correlates with working vs. failing - see Display.h.
    constexpr bool DEBUG_SERIAL = false;

    // One-shot: forces the fuel gauge to discard its corrupted learned FullCapRep
    // and restore from FACTORY_SEED instead. Flip back to false and reflash once
    // confirmed - a software-faked POR doesn't reset the chip's internal MixSOC
    // accumulator, so this alone isn't sufficient; a real battery disconnect/
    // reconnect is still needed to fully recover (see
    // project_fuelgauge_ez_config_calibration memory).
    constexpr bool FORCE_FUELGAUGE_RECAL = false;
} // namespace Config
