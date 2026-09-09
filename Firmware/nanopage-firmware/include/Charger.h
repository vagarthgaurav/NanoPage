// BQ25170 charger status pins (STAT/PG). No I2C - status-only, read directly.

#pragma once

namespace Charger
{
    // Sets up PIN_CHG_STAT/PIN_CHG_PG as inputs; PIN_CHG_STAT gets a CHANGE
    // interrupt that invokes `onStatusChange` (if given) the instant charging
    // starts or stops.
    void begin(void (*onStatusChange)() = nullptr);

    // BQ25170 STAT, not the fuel gauge's current reading: charge current ramps
    // up/down over a couple of seconds around plug/unplug, so a current-threshold
    // test lags the physical event by a few seconds in both directions. STAT
    // reflects the charger's own state and flips within its internal debounce time.
    bool isCharging();

    // Diagnostic only - distinguishes "no real input power" (cable/port issue)
    // from "charger genuinely terminated" while isCharging() is false.
    bool isPowerGood();
} // namespace Charger
