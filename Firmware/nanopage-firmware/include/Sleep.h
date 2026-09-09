// Deep-sleep (SYSTEMOFF) entry sequence - shows a sleep screen, persists fuel
// gauge state, then cuts power until a button wakes the board via a full reset.

#pragma once

class Display;
class FuelGauge;

namespace Sleep
{
    // Never returns - waking from SYSTEMOFF is a full reset that resumes at
    // setup(), not here.
    [[noreturn]] void enterDeepSleep(Display &display, FuelGauge &fuelGauge);
} // namespace Sleep
