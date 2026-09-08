// Blanks the e-paper panel to white and puts its controller into deep-sleep
// (hibernate) storage mode. Run this once on each panel before shelving it -
// leaving a stale image on an e-paper panel for a long period of storage can
// bake it in (ghosting), and hibernating drops the panel's driving voltages
// so it draws no current while it sits unused.
//
// Flash with `pio run -e nanopage_blank -t upload`.

#include <Arduino.h>
#include <SPI.h>

#include "Display.h"

namespace
{
    Display display;
}

void setup()
{
    Serial.begin(115200);
    uint32_t waitStart = millis();
    while (!Serial && (millis() - waitStart) < 3000)
        delay(10);

    Serial.println(F("Display: blanking panel to white and hibernating..."));

    display.begin();
    // fillScreen(WHITE) happens inside refreshFull() before draw() runs, and
    // hibernate() after - passing an empty draw() is enough to just blank it.
    display.refreshFull([]() {});

    Serial.println(F("Display: blanked and hibernating - safe to unplug/store."));
}

void loop()
{
    // Nothing left to do - the panel is blanked and in deep sleep.
}
