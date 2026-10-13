# NanoPage companion protocol (BLE) and the firmware work it needs

Status: **in progress.** Slice 1 (BT stack, companion mode, pairing,
`DEVICE_INFO`, `TIME`) is built. The library service (`LIST`, `CHAPTERS`,
`src/ble/companion_library.c`) is implemented; `LIST` was verified on hardware
(2026-10-14: full scan of the real library completed with an END frame, status
0), while `CHAPTERS`, storage-busy and the empty-card case are still
unverified. `DELETE` is implemented and verified on hardware (2026-10-14): it removes the bookmark sidecar, then the book, and answers
`not found` / `bad request` (bad id, or not a regular file) / `internal`
(unlink failed). Calendar is declared in the GATT table but stubbed.
The wire format is final for `proto_version` 2 and lives in `Firmware/src/ble/companion_proto.h`.

The phone app lives in a separate repo, `NanoPage-companion-app`
(`~/Projects/Software/NanoPage-companion-app`, Flutter, Android and iOS). Its
spec is `docs/spec.md` there. This document is the contract between the two:
the single source of truth for the wire format is a header in this repo,
`Firmware/src/ble/companion_proto.h`, mirrored by hand in a Dart file in the
app repo. Keep the two in sync and bump `proto_version` on any change.

Written 2026-10-13.

## Constraints that shape it

- Battery life is the top product goal. The radio must be **off** outside an
  explicit companion mode (below).
- The device has no time-zone database. All times on the wire are **local
  wall-clock**; the phone converts.
- The device font covers 7-bit ASCII 32 to 126 only (same as the `.npb` text
  blob). Anything shown on the device is transliterated by the phone with the
  same rules as `Converter/transliterate.py`.
- Android and iOS differ in ways that affect the design:
  - iOS never reveals a Bluetooth address. The app identifies the device by
    the platform's peripheral id, which is per phone. The human-readable
    suffix in "NanoPage 4F2A" therefore has to come from the **advertised
    local name** (last 4 hex digits of the nRF52840 device id), not from an
    address.
  - The negotiated MTU differs (iOS decides it itself; typical usable write
    size is well under 247). The app chunks by the MTU the platform reports;
    do not assume 247 on the firmware side either.
  - Pairing UI is drawn by the operating system on both platforms, not by the
    app.

## Pairing and security

LE Secure Connections with bonding. The device declares display-only IO
capability and shows a 6 digit passkey on the e-paper screen; the phone's
system dialog asks for it. Without this anyone nearby could delete books or
push events while the device is in companion mode. All characteristics below
require an encrypted, bonded link. (Confirm with the user: it adds a screen on
the device.)

## GATT layout

One custom service (generate a 128-bit UUID; advertise it so the app can
filter scan results). `DEVICE_INFO` carries a protocol version byte. All
integers little-endian, same as the `.npb` format.

| Characteristic | Properties | Content |
|----------------|-----------|---------|
| `DEVICE_INFO` | read | `proto_version u8`, `fw_version` (string, <= 16), `battery_pct u8`, `charging u8`, `sd_free_kb u32`, `sd_total_kb u32` |
| `TIME` | write | local wall-clock `year u16, month, day, hour, min, sec u8`; sets the PCF85063 RTC |
| `LIB_CTRL` | write | commands: `LIST`, `CHAPTERS(book_id)`, `DELETE(book_id)` |
| `LIB_DATA` | notify | response stream (records below), ended by an end marker carrying a status code |
| `CAL_CTRL` | write | `BEGIN(total_len u32, crc32 u32, event_count u16)`, `COMMIT`, `ABORT` |
| `CAL_DATA` | write without response | payload chunks |
| `CAL_STATUS` | notify | `op u8, status u8`: outcome of `BEGIN` and `COMMIT` |

`LIB_DATA` is a byte stream of frames (`type u8, len u16, payload`), a frame
may span notifications, a response ends with one `END` frame
(`op u8, status u8`). Exact layouts and the UUIDs are in `companion_proto.h`.

Status codes include at least: ok, bad request, storage busy (USB mass storage
or the display owns the SD bus), CRC mismatch, not found, too large.

### Library records (`LIST` response)

One per book: `book_id` (path relative to the SD root, <= 128 bytes, NUL
padded), `title[64]`, `author[64]`, `file_size u32`, `text_blob_size u32`,
`last_position u32`, `chapter_count u16`, `last_read_local u32` (0 if
unknown). Title, author and chapter fields are exactly what `npb_header` and
`npb_chapter` already hold (`Firmware/src/book/npb_format.h`);
`last_position` comes from the bookmark sidecar in `.nanopage/`
(`npb_bookmark_format.h`). `CHAPTERS` returns `{title[64], text_offset u32}`
records. The app computes progress as `last_position / text_blob_size` and
treats >= 99% as finished (there is no finished flag on the device).
The `LIB_CTRL` command for `CHAPTERS`/`DELETE` is therefore 1 + 128 = 129
bytes and needs an MTU of at least 132 (firmware allows 247), because long
(offset) writes are not supported on `LIB_CTRL`. Books whose path exceeds 128
bytes are skipped by `LIST`.

### Calendar payload

One blob written through `CAL_DATA`, then `COMMIT`:

```
header: magic "NPCL", version u16, event_count u16, range_start_local u32
event:  start_local u32   (minutes since 2000-01-01 00:00, local wall clock)
        duration_min u16  (0xFFFF = all day)
        flags u8          (bit0 all_day)
        title_len u8
        title[title_len]  (7-bit ASCII 32..126)
```

Phone-side rules: sorted by start; recurring events expanded; multi-day
events split per day (decide with the day-view spec); count capped (suggest
500) and the app tells the user when events were dropped; titles truncated to
what `calendar_day.c` can show; `TIME` written on every connection and the
list rebuilt when the UTC offset changes (DST).

## Implementation decisions (2026-10-13)

- Bonds are stored with the Zephyr settings subsystem in a new 16 KB
  `settings_partition` (0xF4000), carved out of the app image area (now
  976 KB). `storage_partition` (fuel gauge raw NVS) stays 32 KB: shrinking
  it was tried first and broke the fuel gauge's `nvs_mount` (-45).
- Up to 2 bonded phones. Pairing is only possible while companion mode is
  active, and the user sees the passkey on the screen.
- `bt_enable()` is called when companion mode starts and `bt_disable()` when
  it ends, so the stack is fully down in normal operation.
- 2026-10-14: `book_id` widened from 64 to 128 bytes and `proto_version` bumped
  to 2 (real library paths exceed 64 bytes). The phone app's Dart mirror must
  be updated to match.

## Firmware work

Own `.c`/`.h` per subsystem, verify each piece on real hardware, and do not
commit before the user confirms.

1. **Bluetooth stack**: `CONFIG_BT`, peripheral role, GATT, SMP and bonding
   with display-only IO capability. Check RAM/flash budget and measure idle
   current in System OFF and deep sleep before and after. Do not assume it is
   unchanged.
2. **Companion mode**, never always-on advertising. A new menu entry
   ("Connect to phone", hand-drawn bluetooth icon in the Phosphor style used by
   the others) starts advertising for a limited time (suggest 2 minutes),
   shows status on the e-paper (waiting, passkey, connected, syncing), then
   returns to the normal idle/sleep path. Radio fully off outside this mode.
   Advertised name `NanoPage <last 4 hex of device id>`, plus the service UUID.
3. **Library service**: scan the SD card for `.npb`, read each header, read the
   sidecar for `last_position`. Use the existing SD bring-up/teardown bracket
   and respect USB mass storage and display bus sharing (see the SD backfeed
   notes). If the card is busy return `storage busy` rather than blocking.
4. **Delete** a book and its sidecar. Sidecar goes first so a failed book
   unlink only loses the reading position and never leaves a stale bookmark
   behind. The `book_id` gets the same validation as `CHAPTERS`.
5. **Calendar store**: receive the blob, verify the CRC, write it atomically
   (temp file, then rename) to e.g. `.nanopage/calendar.bin`, and make
   `calendar_day.c` read it instead of its mock events. The calendar day-view
   work still has an open question about where events live; this is the
   proposal.
6. **Time set** from `TIME` into the RTC driver.
7. **Last-read time**: store a timestamp in the sidecar (or use its mtime) so
   "Recent" and "Yesterday, 21:40" in the app can work.

## Test plan (device side)

- Transfer interrupted halfway: the device keeps the old calendar.
- Phone walks out of range mid-sync; USB plugged in mid-sync; empty SD card;
  100+ books.
- Idle current in System OFF and deep sleep with BLE compiled in, compared
  with the current build.
- Pairing with a wrong passkey; a second phone trying to connect to a bonded
  device.
