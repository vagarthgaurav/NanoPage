#include "Buttons.h"

#include <Arduino.h>

namespace
{
    volatile uint32_t lastActivityMs = 0;

    void onActivityEdge()
    {
        lastActivityMs = millis();
    }
} // namespace

namespace Buttons
{
    void begin()
    {
        pinMode(PIN_BTN_PREV, INPUT_PULLUP);
        pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
        pinMode(PIN_BTN_ACTION, INPUT_PULLUP);
        attachInterrupt(PIN_BTN_PREV, onActivityEdge, FALLING | ISR_DEFERRED);
        attachInterrupt(PIN_BTN_NEXT, onActivityEdge, FALLING | ISR_DEFERRED);
        attachInterrupt(PIN_BTN_ACTION, onActivityEdge, FALLING | ISR_DEFERRED);

        lastActivityMs = millis();
    }

    uint32_t msSinceActivity()
    {
        return millis() - lastActivityMs;
    }

    void armWakeOnPress()
    {
        pinMode(PIN_BTN_PREV, INPUT_PULLUP_SENSE);
        pinMode(PIN_BTN_NEXT, INPUT_PULLUP_SENSE);
        pinMode(PIN_BTN_ACTION, INPUT_PULLUP_SENSE);
    }
} // namespace Buttons
