/*
 * NanoPage main firmware.
 *
 * Starting point for real firmware, as opposed to ../peripheral_test's
 * bring-up exerciser. Pull in that project's peripheral-driving code
 * (RTC, fuel gauge, e-paper, microSD, BLE) as this grows.
 */

#include <Arduino.h>
#include <Adafruit_TinyUSB.h> // pulls in the USB CDC Serial object; nothing links it in otherwise

void setup()
{
    Serial.begin(115200);
    uint32_t waited_ms = 0;
    while (!Serial && waited_ms < 3000)
    {
        delay(10);
        waited_ms += 10;
    }

    Serial.println("NanoPage firmware booting");
}

void loop()
{
}
