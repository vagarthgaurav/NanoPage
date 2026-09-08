# NanoPage peripheral test

A standalone PlatformIO firmware that exercises everything on the board
except the microSD card: the PCF85063ATL RTC, the MAX17055 fuel gauge,
the BQ25170 charger's status lines, the TPS2116 power mux's USB detect
line, the Action/Next/Prev buttons, and the GDEY029T94 e-paper panel.
I2C reads are done with raw `Wire` transactions against each chip's
datasheet register map, no library needed; the display uses
[GxEPD2](https://github.com/ZinggJM/GxEPD2) (pulled in via `lib_deps`).

It defines its own custom PlatformIO board (`boards/nanopage.json`) and
Arduino variant (`boards/variants/nanopage/`), since NanoPage isn't an
upstream board and the Adafruit nRF52 core needs a pin table to turn
Arduino pin numbers into physical P0.xx/P1.xx pins. Only the pins this
test uses are mapped — see `boards/variants/nanopage/variant.h`.

## Build & flash

The board needs the Adafruit UF2 bootloader already flashed (see
[../../docs/bootloader.md](../../docs/bootloader.md)), including the
one-time UICR NFCPINS fix — without it the Action/Next buttons won't
read correctly, and this firmware will print a warning at boot if it's
missing.

```bash
cd Firmware/peripheral_test
pio run                # build
# double-tap the reset button to drop into NPAGEBOOT, then:
pio run -t upload
pio device monitor      # 115200 baud
```

## What it prints

- Once at boot: an I2C bus scan, RTC/fuel-gauge presence + identification,
  the NFCPINS warning if applicable, one-time init results (below), and
  the display test (below).
- Every second: RTC date/time (with the oscillator-stop flag decoded),
  fuel gauge cell voltage / state of charge / current (mA, +charging /
  -discharging, via the 10mΩ sense resistor), and charger STAT/PG levels.
- Immediately on change: button presses/releases, RTC/fuel-gauge alert
  line edges, and the power mux's USB-detect line, all debounced ~25 ms.

## One-time bring-up init

On a chip that's never been configured, this firmware also:

- **Seeds the RTC** from the firmware's own build time (`__DATE__`/`__TIME__`)
  if the PCF85063's OS (oscillator-stop) flag is set — i.e. only on a
  factory-fresh chip or one that lost power. It's approximate (doesn't
  account for flash/reset delay) and won't touch a clock that's already
  running; set it properly with a host-time sync once real firmware exists.
- **Runs the MAX17055's EZ Config sequence** (no OCV table) if its POR
  flag is set, using `FUEL_DESIGN_CAPACITY_MAH` in `src/main.cpp`
  (currently 320mAh — update it to match whatever pack is actually
  plugged in for testing) and the 10mΩ `FUEL_RSENSE_OHMS`. Both of
  these only run once per power-on; subsequent boots see the flags
  already clear and leave the chips alone.

## Display test

Shows a live snapshot — title plus RTC date/time, fuel gauge mV/%/mA,
charger status, and USB-detect state — redrawn every `DISPLAY_UPDATE_MS`
in `loop()` (30s by default). Each redraw is a full-window refresh
(~1.75s, visibly flashes) and hibernates the controller again afterwards;
30s is a deliberately slow cadence since e-paper shouldn't be driven with
frequent full refreshes. SPI is shared with the microSD card, so the SD
card's CS and power-enable pins are driven inactive once at startup so it
can't contend with the display on the bus.

## Battery burn test

`platformio.ini` also defines `env:nanopage_burntest`, a separate build
(`src/burn_test_main.cpp`) used only for MAX17055 fuel-gauge calibration -
not needed for normal peripheral bring-up, and not built by default.

```bash
pio run -e nanopage_burntest -t upload
pio device monitor
```

It hammers the SD card and e-paper panel continuously instead of sleeping,
to discharge faster than idle current would allow, and prints
`CYCLE COMPLETE` when a full discharge/charge cycle is detected (or
`SAFE STOP` if the cell hits the low-voltage cutoff first - safe to stop
there too). Reflash `env:nanopage` (or nanopage-firmware's own main
firmware) afterward; this build never sleeps and isn't meant for normal
use. See [../nanopage-firmware/README.md](../nanopage-firmware/README.md#fuel-gauge-factory-seed)
for what the calibration data is used for.

Uses `Display`/`FuelGauge` classes copied from `../nanopage-firmware`
(`include/Display.h`, `include/FuelGauge.h`, `src/Display.cpp`,
`src/FuelGauge.cpp`) rather than this project's usual raw-register style,
since the burn test needs the same EZ-Config/current/voltage logic the
main firmware uses. Not shared via a library - keep them in sync manually
if either project's copy changes.

## Assumptions worth checking against real hardware

- Buttons and the charger/RTC/fuel-gauge/USB-detect signal lines are all
  assumed active-low with pull-ups (external, per schematic, backed by
  the MCU's internal pull-up as well) — swap `INPUT_PULLUP`/`activeLow`
  in `src/main.cpp` if a board revision changes that.
- The reset button isn't exercised here since it's wired to the
  nRF52840's dedicated hardware reset pin, not a GPIO.
