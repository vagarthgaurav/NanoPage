#include "variant.h"
#include "wiring_constants.h"
#include "wiring_digital.h"
#include "nrf.h"

const uint32_t g_ADigitalPinMap[] =
{
   28,      // D0 is P0.28 (I2C SCL)
   32 + 13, // D1 is P1.13 (I2C SDA)
    2,      // D2 is P0.02 (RTC ~INT)
   13,      // D3 is P0.13 (Fuel gauge ALRT)
    3,      // D4 is P0.03 (Charger STAT)
   32 + 0,  // D5 is P1.00 (Charger ~PG)
   24,      // D6 is P0.24 (Prev button)
    9,      // D7 is P0.09 (Action button, NFC1)
   10,      // D8 is P0.10 (Next button, NFC2)
   17,      // D9 is P0.17 (USB detect, TPS2116 ST pin)
   31,      // D10 is P0.31 (SPI SCK)
   30,      // D11 is P0.30 (SPI MOSI)
   29,      // D12 is P0.29 (SPI MISO, SD card only)
   26,      // D13 is P0.26 (EPD chip-select)
    6,      // D14 is P0.06 (EPD data/command)
    5,      // D15 is P0.05 (EPD reset)
    8,      // D16 is P0.08 (EPD busy)
   32 + 9,  // D17 is P1.09 (SD card chip-select)
   32 + 11, // D18 is P1.11 (SD card power enable)
};
