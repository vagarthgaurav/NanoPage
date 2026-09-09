#include "Charger.h"

#include <Arduino.h>

namespace
{
    void (*g_onStatusChange)() = nullptr;

    void onStatusEdge()
    {
        if (g_onStatusChange)
            g_onStatusChange();
    }
} // namespace

namespace Charger
{
    void begin(void (*onStatusChange)())
    {
        g_onStatusChange = onStatusChange;

        pinMode(PIN_CHG_STAT, INPUT_PULLUP); // open-drain
        attachInterrupt(PIN_CHG_STAT, onStatusEdge, CHANGE | ISR_DEFERRED);

        pinMode(PIN_CHG_PG, INPUT_PULLUP); // open-drain, diagnostic only for now (see isPowerGood())
    }

    bool isCharging()
    {
        return digitalRead(PIN_CHG_STAT) == LOW;
    }

    bool isPowerGood()
    {
        return digitalRead(PIN_CHG_PG) == LOW;
    }
} // namespace Charger
