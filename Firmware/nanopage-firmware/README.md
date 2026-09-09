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
constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x003E, 0x223E, 0x02B2, 0x0277, 0x0031};
constexpr bool FACTORY_SEED_VALID = true;
```

Order: `rcomp0, tempCo, fullCapNom, fullCapRep, cycles`. Captured
2026-09-09 from the `CYCLE COMPLETE` register dump at the end of a full
burn-test discharge/charge cycle, after fixing a bug where `DesignCap`/
`IChgTerm` weren't being re-written on every boot (see
`project_fuelgauge_ez_config_calibration` session notes) - the previous
seed captured 2026-09-08 predates that fix and may have inherited the
same corruption. `fullCapRep` (0x0277 = 315.5mAh) is close to
`DESIGN_CAPACITY_MAH` (320mAh) and `cycles` (0x0031 = 49) reflects
real convergence over roughly half a cycle, both confirmed sane via
`MixSOC` also being in-range (previously seen as high as 164%, an
impossible value, when the underlying bug was present).

### Recapturing it on a new reference board

The burn test firmware lives in `../peripheral_test` (`env:nanopage_burntest`
there), not in this project - see
[../peripheral_test/README.md](../peripheral_test/README.md#battery-burn-test)
for how to run it. `burn_test_main.cpp` automatically prints a full
register dump (`FuelGauge::debugDumpRegisters()`) to serial at both the
`DEPLETED` and `CHARGED` on-screen events, so no manual instrumentation
is needed - just watch the serial monitor for the `CHARGED` dump at the
end of a cycle and read `RCOMP0`, `TempCo`, `FullCapNom`, `FullCapRep`,
and `Cycles` straight off it. Paste them into `FACTORY_SEED` above, in
that order, and update this section.
