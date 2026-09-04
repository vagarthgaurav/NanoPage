# Flashing the Bootloader

NanoPage uses the [Adafruit nRF52 Bootloader](https://github.com/adafruit/Adafruit_nRF52_Bootloader)
on its E73-2G4M08S1C (nRF52840) module. It's flashed once over SWD via a
Raspberry Pi Debug Probe; after that, application firmware can be updated
over USB by dragging a `.uf2` file onto the `NPAGEBOOT` drive that appears
when the board is double-reset (or the bootloader has no valid app yet).

This board has no status LED and no NFC antenna, both of which needed
non-default handling — see below.

## Prerequisites

- Raspberry Pi Debug Probe wired to the board's `SWDIO` / `SWCLK` / `GND` pads.
- `arm-none-eabi-gcc` (build the bootloader)
- `openocd` (flash over SWD) — installed via `sudo apt-get install openocd`
- `git`

### One-time udev rule for the Debug Probe

The Debug Probe (USB VID:PID `2e8a:000c`) isn't accessible to a regular user
by default. PlatformIO ships a udev rule that already covers it, so if
PlatformIO is installed, just deploy that file instead of writing one from
scratch:

```bash
sudo cp ~/.platformio/penv/lib/python3.10/site-packages/platformio/assets/system/99-platformio-udev.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and replug the probe afterwards.

Sanity check the SWD link before doing anything else:

```bash
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg -c "init" -c "targets" -c "exit"
```

You should see `nrf52.cpu` listed as `running`.

## 1. Get the bootloader source

```bash
cd Firmware
git clone --recurse-submodules https://github.com/adafruit/Adafruit_nRF52_Bootloader.git
cd Adafruit_nRF52_Bootloader
# if any submodule (e.g. lib/uf2) ends up empty:
git submodule update --init --recursive
```

## 2. Custom board port: `src/boards/nanopage/`

The upstream repo has no NanoPage board definition, so one was added,
modeled on `electronut_labs_papyr` (also an e-paper reader) and
`ebyte_e73_tbb` (same module family). Pin assignments came straight from
the schematics / `NanoPage.kicad_pcb` net list for the `U8` (E73-2G4M08S1C)
footprint:

| Signal | nRF52840 pin | Net |
|---|---|---|
| SWDIO | dedicated `SWDIO` pin (pad 37) | `MCU_SWDIO` |
| SWCLK | dedicated `SWCLK` pin (pad 39) | `MCU_SWCLK` |
| Reset button | P0.18 (pad 26) | `RESET_BTN` (dedicated hw reset pin) |
| Action button | P0.09/NFC1 (pad 41) | `ACTION_BTN` |
| Next button | P0.10/NFC2 (pad 43) | `NEXT_BTN` |
| Prev button | P0.24 (pad 35) | `PREV_BTN` |
| USB D+/D- | pads 31/29 | `MCU_D+`/`MCU_D-` |
| USB detect | P0.17 (pad 30) | `MCU_USB_DETECT` |

Files added:

- `board.h` — pin/USB descriptor config (see notes below)
- `board.mk` / `board.cmake` — set `MCU_SUB_VARIANT`/`MCU_VARIANT = nrf52840`
- `pinconfig.c` — bootloader config block (flash/RAM size, UF2 family ID), copied from `electronut_labs_papyr`

### Notes / decisions baked into `board.h`

- **No LED on this board.** `LED_PRIMARY_PIN` points at P0.15, which is
  otherwise unconnected, purely so the build has a pin to reference. It
  won't visibly do anything.
- **`BUTTON_DFU` = P0.09 (`ACTION_BTN`).** Holding this button while
  resetting forces the UF2/CDC bootloader instead of jumping to the app.
  Double-tapping the physical reset button (wired to the chip's dedicated
  `P0.18` reset pin) also works, no extra config needed for that path.
- **USB VID/PID are placeholders**: `0x239A` (Adafruit's VID, reused the
  same way every other third-party board in this repo does — fine for
  personal use, not for commercial distribution) with PID `0x8071` (UF2)
  / `0x8072` (CDC-only). Revisit if this ever needs its own registered VID.
- **`BLEDIS_MANUFACTURER`/`BLEDIS_MODEL`/`UF2_INDEX_URL`** were required by
  the build (BLE OTA DIS strings, and the redirect page shown when the
  bootloader drive's `INDEX.HTM` is opened) — set to "NanoPage" and this
  repo's GitHub URL.

## 3. Build

```bash
cd Firmware/Adafruit_nRF52_Bootloader
make BOARD=nanopage all
```

Output lands in `_build/build-nanopage/`. The file that matters for a
first flash on a blank chip is the merged image, which bundles the
bootloader with the SoftDevice (S140 v6.1.1) and MBR:

```
nanopage_bootloader-<version>_s140_6.1.1.hex
```

(`*_nosd.hex` is bootloader-only, useful for re-flashing just the
bootloader on a chip that already has a SoftDevice; `update-*.uf2` is for
updating the bootloader itself later via the UF2 drive.)

## 4. Disable NFC pin mode (one-time, per chip)

`ACTION_BTN` and `NEXT_BTN` sit on P0.09/P0.10, which the nRF52840 defaults
to NFC antenna mode via a UICR register — not GPIO. Since this board has no
NFC antenna, that register needs to be cleared once so those pins behave as
normal digital inputs (this matters for any firmware, not just the
bootloader). On a factory-fresh chip (UICR = `0xFFFFFFFF` everywhere) this
is a single-bit clear and doesn't require erasing the rest of UICR:

```bash
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg \
  -c "init" -c "reset halt" \
  -c "mww 0x4001e504 0x1" \
  -c "mww 0x1000120C 0xfffffffe" \
  -c "mww 0x4001e504 0x0" \
  -c "sleep 10" \
  -c "mdw 0x1000120C" \
  -c "exit"
```

Confirm the readback shows `0x1000120c: fffffffe` (bit 0 cleared) before
moving on. If you ever mass-erase the chip, this needs to be redone
afterwards — mass erase wipes UICR back to `0xFFFFFFFF`.

## 5. Flash

```bash
cd Firmware/Adafruit_nRF52_Bootloader
HEX=_build/build-nanopage/nanopage_bootloader-<version>_s140_6.1.1.hex
openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg \
  -c "init" -c "reset halt" \
  -c "program $HEX verify" \
  -c "reset run" \
  -c "exit"
```

OpenOCD's flash driver preserves existing UICR bytes it isn't writing
(read-modify-erase-write per page), so this does not undo the NFCPINS
change from step 4 as long as step 4 ran first.

## 6. Verify

- SWD-only check (no need to touch the board's own USB port): reset and
  read the program counter — it should land somewhere in the bootloader's
  flash region (starts at `0xF4000` on this layout):

  ```bash
  openocd -f interface/cmsis-dap.cfg -c "transport select swd" -f target/nrf52.cfg \
    -c "init" -c "reset halt" -c "reg pc" -c "resume" -c "exit"
  ```

- Real check: plug NanoPage's own USB-C port into a PC (the Debug Probe
  only carries SWD, not USB data). With no application flashed yet, the
  bootloader stays resident and a mass-storage drive named `NPAGEBOOT`
  should appear — confirmed working on the first flash.

## Re-flashing later

- **New bootloader build**: repeat step 5 with the fresh hex. Step 4
  (NFCPINS) only needs to be redone if the chip gets mass-erased.
- **Application firmware**: once there's an app, drag its `.uf2` onto the
  `NPAGEBOOT` drive, or double-tap reset to re-enter the bootloader first.
