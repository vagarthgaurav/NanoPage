// Prev/Next/Action button inputs.

#pragma once

#include <cstdint>

namespace Buttons
{
    // Sets up PIN_BTN_PREV/NEXT/ACTION as inputs with FALLING-edge interrupts.
    // Each callback (optional) fires after the activity clock is updated.
    void begin(void (*onPrev)() = nullptr, void (*onNext)() = nullptr, void (*onAction)() = nullptr);

    // Milliseconds since the last button press, or since begin() if none yet.
    uint32_t msSinceActivity();

    // Reconfigures the button pins as SYSTEMOFF wake sources. Call right before
    // NRF_POWER->SYSTEMOFF - waking from it is a full reset, so there's no
    // matching "undo" call on the other side.
    void armWakeOnPress();
} // namespace Buttons
