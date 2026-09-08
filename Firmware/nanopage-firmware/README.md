# NanoPage main firmware

## Build & flash

The board needs the Adafruit UF2 bootloader already flashed (see
[../docs/bootloader.md](../docs/bootloader.md)).

```bash
cd Firmware/nanopage-firmware
pio run -e nanopage          # build
# double-tap the reset button to drop into NPAGEBOOT, then:
pio run -e nanopage -t upload
pio device monitor            # 115200 baud
```

## Fuel gauge factory seed

The MAX17055 fuel gauge learns a model of the battery cell (internal
resistance, temperature compensation, full capacity) as it charges and
discharges - that state lives only in the chip's own registers and is
lost if the battery is ever unplugged. `FuelGauge::LearnedParams`
mirrors it to internal flash on a timer and before deep sleep
(`src/main.cpp`), so a power loss on a board that's already run for a
while doesn't mean relearning from scratch.

A brand-new board has no flash save yet either, so `FACTORY_SEED` in
`src/main.cpp` seeds it with a reference snapshot instead of starting
from the generic built-in Li-ion model - every board uses the same
EEMB LP402535 pack, so one calibrated reference board's learned
parameters are a reasonable starting point for all of them. Normal use
keeps self-correcting the model from there.

```cpp
// src/main.cpp
constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x0013, 0x223E, 0x02E6, 0x0280, 0x0013};
constexpr bool FACTORY_SEED_VALID = true;
```

Order: `rcomp0, tempCo, fullCapNom, fullCapRep, cycles`. Captured
2026-09-08 from a board run through one burn test cycle (see below);
`fullCapRep` (0x0280 = 320mAh) matched `DESIGN_CAPACITY_MAH` exactly,
though `cycles` (0x0013 = 19) means the model had only converged over
about a fifth of a full cycle - recapturing from a board with more
cycles on it would be a better long-term reference.

### Recapturing it on a new reference board

The burn test firmware lives in `../peripheral_test` (`env:nanopage_burntest`
there), not in this project - see
[../peripheral_test/README.md](../peripheral_test/README.md#battery-burn-test)
for how to run it. Once it's done:

1. Reflash this project's main firmware: `pio run -e nanopage -t upload`.
2. In `FuelGauge::begin()` (`src/FuelGauge.cpp`), temporarily add a
   `Serial.print` of `learnedParams()` (or call it from `setup()`
   after `fuelGauge.begin()`) and read the five values off the serial
   monitor.
3. Paste them into `FACTORY_SEED` above, in the same order, and update
   this section.
