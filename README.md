# NanoPage

A custom e-reader board built around the Nordic nRF52840, in the form of an
Ebyte E73-2G4M08S1C module.

## Features

- Ebyte E73-2G4M08S1C module (Nordic nRF52840): Cortex-M4F, Bluetooth Low
  Energy, USB 2.0 full-speed
- E-paper (E Ink) panel via a 24-pin FPC connector, with its own onboard
  boost/negative-voltage generation (synchronous switcher, Schottky diode,
  inductor) to produce the panel's bias rails. It shares an SPI bus with
  the SD card, each with its own chip-select line.
- microSD card slot on the same SPI bus, power-gated by a load switch so
  it can be fully powered down between reads
- PCF85063ATL real-time clock on I2C with its own 32.768 kHz crystal and
  an interrupt line back to the MCU, for scheduling wake-ups while the MCU
  sleeps
- Power management: USB-C input for charging and USB data with ESD
  protection on the data lines, a linear Li-Po charger (BQ25170) and an
  impedance-track fuel gauge (MAX17055) sharing the I2C bus with the RTC,
  an automatic power-source mux between USB and battery, a pair of
  nanopower LDOs for the low-power rails, and a 2-pin JST battery
  connector
- Three tactile buttons (Action, Next, Prev), a dedicated reset button,
  and a 7-pin header for external or case-mounted buttons
- 3-pin JST-SH SWD connector (SWDIO / SWCLK / GND), wired to plug
  directly into a Raspberry Pi Debug Probe
- 6-layer PCB

See [docs/bootloader.md](docs/bootloader.md) for flashing the bootloader
over SWD.

## Pinout

Pin numbers below are the E73-2G4M08S1C module's physical pins (`U8` in
the schematic), not raw nRF52840 die pins.

| Module pin | nRF52840 GPIO | Function |
|---|---|---|
| 1  | P1.11        | SD card power enable |
| 2  | P1.10        | NC |
| 3  | P0.03/AIN1   | Charger status |
| 4  | P0.28/AIN4   | I2C SCL (RTC + fuel gauge) |
| 5  | GND          | Ground |
| 6  | P1.13        | I2C SDA (RTC + fuel gauge) |
| 7  | P0.02/AIN0   | RTC interrupt |
| 8  | P0.29/AIN5   | SPI MISO (display + SD card) |
| 9  | P0.31/AIN7   | SPI SCLK (display + SD card) |
| 10 | P0.30/AIN6   | SPI MOSI (display + SD card) |
| 11 | P0.00/XL1    | 32.768 kHz crystal |
| 12 | P0.26        | Display chip-select |
| 13 | P0.01/XL2    | 32.768 kHz crystal |
| 14 | P0.06        | Display data/command |
| 15 | P0.05/AIN3   | Display reset |
| 16 | P0.08        | Display busy |
| 17 | P1.09        | SD card chip-select |
| 18 | P0.04/AIN2   | NC |
| 19 | VDD          | +3.3V |
| 20 | P0.12        | SD card detect |
| 21 | GND          | Ground |
| 22 | P0.07        | NC |
| 23 | VDDH         | +3.3V |
| 24 | GND          | Ground |
| 25 | DCC          | DC/DC decoupling (internal) |
| 26 | P0.18/RESET  | Reset button |
| 27 | VBUS         | USB VBUS |
| 28 | P0.15        | NC |
| 29 | USB D-       | USB data |
| 30 | P0.17        | USB detect |
| 31 | USB D+       | USB data |
| 32 | P0.20        | NC |
| 33 | P0.13        | Fuel gauge alert |
| 34 | P0.22        | NC |
| 35 | P0.24        | Prev button |
| 36 | P1.00        | Charger power-good |
| 37 | SWDIO        | SWD data |
| 38 | P1.02        | NC |
| 39 | SWDCLK       | SWD clock |
| 40 | P1.04        | NC |
| 41 | P0.09/NFC1   | Action button |
| 42 | P1.06        | NC |
| 43 | P0.10/NFC2   | Next button |

P0.09 and P0.10 default to NFC antenna mode on the nRF52840. This board
has no NFC antenna, so firmware needs to switch those pins to GPIO mode
via UICR before the Action and Next buttons will read correctly (see
[docs/bootloader.md](docs/bootloader.md) for how this was done for the
bootloader).
