/*
 * PlatformIO/Arduino variant for NanoPage (Ebyte E73-2G4M08S1C / nRF52840).
 *
 * Copied from ../../../../peripheral_test/boards/variants/nanopage/ as the
 * starting pin map. Arduino pin numbers below are plain array indices into
 * g_ADigitalPinMap (variant.cpp), which is where the physical P0.xx/P1.xx
 * assignment lives. See ../../../../../README.md's pinout table for the
 * source of truth, and extend this file as this firmware grows to touch
 * more of the board than peripheral_test did.
 */

#ifndef _VARIANT_NANOPAGE_
#define _VARIANT_NANOPAGE_

#define VARIANT_MCK (64000000ul)

#define USE_LFXO // NanoPage has its own 32.768kHz crystal

#include "WVariant.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PINS_COUNT       (19)
#define NUM_DIGITAL_PINS (19)
#define NUM_ANALOG_INPUTS  (0)
#define NUM_ANALOG_OUTPUTS (0)

// NanoPage has no status LED; the core's ledOn()/ledOff() helpers still
// need this to compile even though nothing on this board uses them.
#define LED_STATE_ON 1

// Likewise no LED_BUILTIN/LED_BLUE - but Adafruit's InternalFileSystem
// (pulled in transitively by Bluefruit for BLE bond storage) calls
// ledOn(LED_BUILTIN) unconditionally, and Bluefruit itself unconditionally
// references LED_BLUE for its connection-status blink, so both macros
// still have to exist to compile. Point them past PINS_COUNT:
// pinMode()/digitalWrite() bounds-check against PINS_COUNT and silently
// no-op for an out-of-range pin, so neither can glitch a real, wired-up
// GPIO if either code path actually runs.
#define LED_BUILTIN PINS_COUNT
#define LED_BLUE (PINS_COUNT + 1)

// I2C bus shared by the RTC (PCF85063ATL) and fuel gauge (MAX17055)
#define PIN_WIRE_SDA (1) // P1.13
#define PIN_WIRE_SCL (0) // P0.28
#define WIRE_INTERFACES_COUNT 1

// NanoPage has no UART header; the Adafruit core unconditionally
// instantiates a global Serial1 object for non-nRF52832 parts, so these
// just need to be *some* valid index. Serial1 is never begin()'d.
#define PIN_SERIAL1_RX (0)
#define PIN_SERIAL1_TX (0)

// RTC interrupt (open-drain, active low)
#define PIN_RTC_INT (2) // P0.02

// Fuel gauge ALRT (open-drain, active low)
#define PIN_FUEL_ALERT (3) // P0.13

// BQ25170 charger status pins (open-drain, active low, external pull-ups)
#define PIN_CHG_STAT (4) // P0.03 - LOW while charging
#define PIN_CHG_PG   (5) // P1.00 - LOW while input power is present/good

// Tactile buttons (switch to GND when pressed)
#define PIN_BTN_PREV   (6) // P0.24
#define PIN_BTN_ACTION (7) // P0.09 - NFC1 pin, needs UICR NFCPINS = GPIO
#define PIN_BTN_NEXT   (8) // P0.10 - NFC2 pin, needs UICR NFCPINS = GPIO

// TPS2116 power-mux ST pin: reads HIGH while VIN1 (USB) is the
// qualified/selected source, LOW when running from VIN2 (battery)
// instead. (Opposite of the TPS2116 datasheet's usual ST polarity -
// confirmed against this board by observed behavior.)
#define PIN_USB_DETECT (9) // P0.17

// SPI bus shared by the e-paper panel and the microSD card, each with
// its own chip-select. MISO is only wired to the SD card (the panel is
// write-only, no readback), but SPIClass::begin() still needs a pin.
#define PIN_SPI_SCK  (10) // P0.31
#define PIN_SPI_MOSI (11) // P0.30
#define PIN_SPI_MISO (12) // P0.29
#define SPI_INTERFACES_COUNT 1

// GDEY029T94 e-paper panel (SSD1680, 128x296)
#define PIN_EPD_CS   (13) // P0.26
#define PIN_EPD_DC   (14) // P0.06
#define PIN_EPD_RST  (15) // P0.05
#define PIN_EPD_BUSY (16) // P0.08

// Conventional Arduino SPI pin aliases - some libraries (including parts
// of GxEPD2 that PlatformIO's dependency finder pulls in for panels this
// board doesn't have) reference these directly instead of PIN_SPI_*.
static const uint8_t SS = PIN_EPD_CS;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

// microSD card - not exercised by this test, but its CS/power-enable
// need to be driven inactive so it doesn't contend with the display on
// the shared SPI bus.
#define PIN_SD_CS (17) // P1.09
#define PIN_SD_ON (18) // P1.11 - TPS22929D load switch, active high

#ifdef __cplusplus
}
#endif

#endif // _VARIANT_NANOPAGE_
