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
lost whenever the battery is unplugged (or on a brand-new board that's
never seen one). `src/main.cpp` mirrors it to internal flash
(`/fuelgauge_learned.bin`, checksummed, saved hourly and before deep
sleep) so a real power cycle can reload this exact chip/battery's own
history instead of starting over.

Whenever `FuelGauge::begin()` sees the chip's own POR flag set (a real
power cycle, or a fresh/never-configured chip), it seeds the restore
with, in priority order: this chip's own flash-saved history if present
and it passes a plausibility check (`FuelGauge::learnedParamsPlausible()`
- rejects a wildly-out-of-range capacity value, since a corrupted save
faithfully reloaded forever was the root cause of most of this project's
fuel-gauge saga); otherwise `FACTORY_SEED`, a reference snapshot from a
calibrated board with the same EEMB LP402535 pack, for a genuinely new
board with no history yet. Either way, a full EZ-Config model load
always runs first on a real POR before the learned params are layered
on top - see `project_fuelgauge_ez_config_calibration` memory for why
that ordering matters (it's what actually initializes `MixSOC`).

To manually wipe a chip's learned history and force a fresh start from
`FACTORY_SEED` (e.g. after swapping in a new battery), set
`Config::FORCE_FUELGAUGE_RECAL = true` and reflash; it deletes the flash
snapshot and forces a restore from `FACTORY_SEED`, though a real battery
disconnect/reconnect afterward is still the most reliable way to get a
fully clean state (a software-faked POR doesn't reset the chip's
internal `MixSOC` accumulator on its own). Flip it back to `false` and
reflash once confirmed.

```cpp
// src/main.cpp
constexpr FuelGauge::LearnedParams FACTORY_SEED = {0x003E, 0x223E, 0x02B2, 0x0277, 0x0031};
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
