// Prev/Next/Action button inputs. Currently just tracks activity for the
// sleep-timeout idle check - no navigation feature consumes button presses yet.

#pragma once

#include <cstdint>

namespace Buttons
{
    // Sets up PIN_BTN_PREV/NEXT/ACTION as inputs with FALLING-edge interrupts,
    // and resets the activity clock.
    void begin();

    // Milliseconds since the last button press, or since begin() if none yet.
    uint32_t msSinceActivity();

    // Reconfigures the button pins as SYSTEMOFF wake sources. Call right before
    // NRF_POWER->SYSTEMOFF - waking from it is a full reset, so there's no
    // matching "undo" call on the other side.
    void armWakeOnPress();
} // namespace Buttons
