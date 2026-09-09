#include "Buttons.h"

#include <Arduino.h>

namespace
{
    volatile uint32_t lastActivityMs = 0;
    void (*prevCallback)() = nullptr;
    void (*nextCallback)() = nullptr;
    void (*actionCallback)() = nullptr;

    void onPrevEdge()
    {
        lastActivityMs = millis();
        if (prevCallback)
            prevCallback();
    }

    void onNextEdge()
    {
        lastActivityMs = millis();
        if (nextCallback)
            nextCallback();
    }

    void onActionEdge()
    {
        lastActivityMs = millis();
        if (actionCallback)
            actionCallback();
    }
} // namespace

namespace Buttons
{
    void begin(void (*onPrev)(), void (*onNext)(), void (*onAction)())
    {
        prevCallback = onPrev;
        nextCallback = onNext;
        actionCallback = onAction;

        pinMode(PIN_BTN_PREV, INPUT_PULLUP);
        pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
        pinMode(PIN_BTN_ACTION, INPUT_PULLUP);
        attachInterrupt(PIN_BTN_PREV, onPrevEdge, FALLING | ISR_DEFERRED);
        attachInterrupt(PIN_BTN_NEXT, onNextEdge, FALLING | ISR_DEFERRED);
        attachInterrupt(PIN_BTN_ACTION, onActionEdge, FALLING | ISR_DEFERRED);

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
