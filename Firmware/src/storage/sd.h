/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SD_H_
#define APP_SD_H_

#include <stdbool.h>
#include <stddef.h>

/*
 * IMPORTANT hardware constraint - the microSD card is a genuine
 * bus-sharing hazard on this board (no bus-isolation switch, only its
 * own VDD is gated), and there are TWO separate backfeed mechanisms
 * confirmed on real hardware, not one - easy to conflate, worth reading
 * both before touching this file again:
 *
 * 1. STATIC backfeed via CS, while spi3 sits idle (found/fixed
 *    2026-09-13): the card's own CS line (P1.09) is left at a fixed
 *    level by Zephyr's SPI driver whenever nothing is transacting -
 *    deasserted (physically HIGH) by default - which forward-biases
 *    the card's CS-pin ESD diode into its unpowered VDD rail
 *    (+3V3_SD). Measured 0.827V at rest (one diode drop) and a steady
 *    ~7mA idle draw before the fix; could also leave the board unable
 *    to refresh after a real battery power cycle. Fixed in
 *    app_sd_power_off() below: CS is forced to its active-low
 *    ("asserted", physically LOW) level *before* the regulator is
 *    disabled. Confirmed on hardware: +3V3_SD now sits at a clean 0V
 *    at rest. A boot-time hook that additionally forced CS low from
 *    the very first instant of boot was tried and reverted the same
 *    day - see the long comment on sd_cs_gpio in sd.c for why. The
 *    brief window between spi3's own init (which leaves CS deasserted/
 *    HIGH) and the first app_sd_power_on() call is apparently harmless
 *    in practice - no bus traffic happens during it - so this mechanism
 *    is fully covered by app_sd_power_off()'s fix alone.
 *
 * 2. ACTIVE backfeed via SCK/MOSI, during an actual refresh (the
 *    *original* freeze bug from earlier bring-up, re-confirmed
 *    2026-09-13): while the display is actively driving spi3 for a
 *    refresh, SCK/MOSI toggle regardless of what CS is doing, and
 *    those same shared lines forward-bias the unpowered card's own
 *    ESD diodes just the same way CS does at rest - CS's level is
 *    irrelevant here, since the corruption path doesn't go through
 *    CS at all. There is no static fix for this (short of a hardware
 *    bus-isolation switch) - the only thing that works is having the
 *    card genuinely powered for the duration of the refresh, which is
 *    why app_display_lock()/app_display_unlock() in display.c - taken
 *    by every refresh function - bracket the refresh with
 *    app_sd_power_on()/app_sd_power_off() below. Confirmed by removing
 *    this bracket on 2026-09-13: refreshes broke again immediately
 *    with the card inserted, even with mechanism 1's fix still in
 *    place - proving the two mechanisms are independent and both need
 *    their own fix.
 *
 * Net effect: the card's power is reference-counted (see
 * app_sd_power_on()/app_sd_power_off() below) so app_sd_init()'s own
 * longer-lived hold (for an actual mount/read) composes correctly with
 * display.c's much shorter nested hold around every single refresh,
 * regardless of which one asked for power first.
 */

/*
 * Disk name the SD card is registered under (nanopage.dts's disk-name
 * property, and the FATFS mount point below - "/" APP_SD_DISK_NAME ":").
 * Shared with usb_msc.c so its USBD_DEFINE_MSC_LUN() and this file's own
 * disk_access_*() calls (app_sd_usb_claim()/app_sd_usb_release() below)
 * can't drift apart into naming two different disks.
 */
#define APP_SD_DISK_NAME "SD"

/*
 * Powers the microSD card's load switch (regulator_sdhc, P1.11 - boots
 * off per nanopage.dts), waits for the card to settle, then mounts its
 * FAT filesystem. Returns 0 on success, a negative errno otherwise.
 *
 * This function holds display.c's app_display_lock()/app_display_unlock()
 * for its whole duration (power-on through mount), so it can't run
 * concurrently with a display refresh - callers don't need to sequence
 * around this themselves.
 *
 * The card is left powered and mounted on return, independently of
 * the per-refresh power bracketing in display.c described above -
 * call app_sd_deinit() once truly done with the card (see below).
 *
 * Returns -EBUSY immediately, without touching power/CS/mount at all, if
 * app_sd_usb_claim() (below) currently has the card reserved for USB Mass
 * Storage - every caller in this app (browser.c, reader.c, currentlog.c)
 * already treats a failed app_sd_init() as "card not available right
 * now" and degrades gracefully, so this is enough to keep local access
 * from racing a live USB session for the shared regulator/CS/bus state.
 */
int app_sd_init(void);

/*
 * Unmounts the FAT filesystem (if mounted) and releases app_sd_init()'s
 * power hold - safe to call even if app_sd_init() was never called or
 * failed. Also holds the display lock for its whole duration, for the
 * same reason as app_sd_init() above.
 */
void app_sd_deinit(void);

/*
 * Hands the microSD card to USB Mass Storage (usb_msc.c) for the duration
 * of a host connection: powers the card on (same regulator sequencing as
 * app_sd_init()) and initializes the disk_access layer directly under
 * APP_SD_DISK_NAME - no FAT mount, since USB MSC exposes the raw
 * disk_access block device straight to the host rather than going through
 * Zephyr's FS layer, and mounting locally at the same time would give the
 * app's own FatFs cache and the host's independent view of the volume two
 * unsynchronized copies of the same on-disk structures.
 *
 * Also makes every app_sd_init() call fail with -EBUSY (see above) for as
 * long as this hold is active.
 *
 * Unlike every other caller in this file, this holds display.c's
 * app_display_lock() for the *entire* session rather than releasing it
 * before returning - the actual SD block reads/writes during a live MSC
 * session come from Zephyr's own MSC thread, asynchronously, with no
 * app_display_lock() bracket of their own, so spi3 needs to stay resumed
 * continuously for as long as USB owns the card (see spi3_pm_refcount in
 * display.c for the mechanism that makes this compose correctly with the
 * periodic footer/header refresh's own much briefer nested holds).
 * app_sd_usb_release() below releases it.
 *
 * Returns 0 on success, negative errno if the card couldn't be powered or
 * initialized (e.g. no card inserted) - the app_sd_init() guard is set
 * either way, since the card stays reserved for USB for the rest of this
 * connection regardless of whether there's currently anything on it to
 * serve. On failure, power is deliberately left on rather than cut - same
 * "inserted-but-unpowered corrupts the shared bus" reasoning as
 * app_sd_init()'s identical choice on a failed fs_mount() above;
 * app_sd_usb_release() below powers it down once the session actually
 * ends.
 */
int app_sd_usb_claim(void);

/*
 * Releases app_sd_usb_claim()'s hold: deinitializes the disk_access layer
 * and powers the card off (same CS-low-before-power-down sequencing as
 * app_sd_power_off()), then clears the app_sd_init() -EBUSY guard. Safe
 * to call even if app_sd_usb_claim() was never called or failed.
 */
void app_sd_usb_release(void);

/*
 * Reference-counted power control for the card's load switch, decoupled
 * from app_sd_init()/app_sd_deinit()'s mount state - the first call
 * that takes the count from 0 actually enables the regulator and waits
 * for the card to settle (see app_sd_init()'s power-up delay); the call
 * that drops it back to 0 actually disables it (forcing CS low first -
 * see mechanism 1 in the policy note above). display.c's
 * app_display_lock()/app_display_unlock() call these around every
 * refresh (mechanism 2 above); app_sd_init()/app_sd_deinit() hold
 * their own independent, longer-lived reference through these same
 * functions rather than touching the regulator directly, so the two
 * uses compose correctly instead of one cutting power out from under
 * the other. Only meant to be called from within this file and
 * display.c - not part of the normal app-facing API surface, but can't
 * be made private (static) since display.c needs it too.
 */
int app_sd_power_on(void);
void app_sd_power_off(void);

/*
 * Generous shared bound for a full SD path (mount point + subdirs), used
 * both internally (app_sd_list_dir()'s own working buffer below) and by
 * any caller building up a "current directory" string while navigating
 * (e.g. a future file browser) - the old Arduino-era firmware had a real
 * path-buffer overflow bug from descending deeply-nested directories with
 * an undersized fixed buffer (see git history, "prevent path_ buffer
 * overflow when descending deeply-nested dirs"); sharing one generous
 * constant instead of every caller guessing its own is cheap insurance
 * against repeating that.
 */
#define APP_SD_MAX_PATH_LEN 200

/*
 * Longest filename app_sd_list_dir() below will return, INCLUDING the
 * null terminator - longer names are truncated, not rejected. FAT/LFN on
 * this card can return names up to MAX_FILE_NAME (zephyr/fs/fs.h,
 * currently 255 per CONFIG_FS_FATFS_MAX_LFN in prj.conf), but nothing
 * that displays these (text.c's character grid is well under 50 columns
 * even at the smallest UI font) needs the full length, and a fixed,
 * shorter cap keeps struct app_sd_dirent cheap enough to hold a whole
 * directory's worth of entries in one static/stack array.
 */
#define APP_SD_MAX_NAME_LEN 48

struct app_sd_dirent {
	char name[APP_SD_MAX_NAME_LEN];
	bool is_dir;
};

/*
 * Lists the entries of `path` (a directory, relative to the SD
 * filesystem's own root and starting with '/' - e.g. "/" or "/books", NOT
 * prefixed with the internal FATFS mount point) into the caller-provided
 * `entries` array, up to `max_entries`. Hidden/dot entries (on FAT/LFN
 * this is really only "." and ".." themselves - fs_readdir() never
 * returns other dot-prefixed names on FAT) are skipped, not counted.
 * `*count` is always set to how many entries were actually written,
 * including 0 if the directory was empty or every entry was skipped.
 *
 * Mechanism only, no policy: entries come back in whatever order
 * fs_readdir() itself returns them (raw FAT directory order - not
 * sorted, directories and files interleaved). Sorting, putting
 * directories first, and paging past max_entries are all a browser UI's
 * job, not this file's - same mechanism/policy split as max17055.c
 * (raw register access) vs fuel_gauge.c (what to do with it).
 *
 * Requires the card to already be mounted - i.e. called while an
 * app_sd_init() hold is still active. Returns -ENODEV immediately if
 * not, rather than mounting or touching the power switch/CS pin itself
 * (see the two-mechanism backfeed writeup above for why those stay
 * confined to app_sd_init()/app_sd_deinit()/app_sd_power_on()/
 * app_sd_power_off()). Every caller in this app (browser.c's load_dir(),
 * one call per listing) brackets each individual call with its own
 * app_sd_init()/app_sd_deinit() pair rather than holding one long-lived
 * mount across a whole browsing session - a deliberate choice to keep
 * the card genuinely powered off between navigations/page turns, at the
 * cost of re-paying SD_POWER_UP_DELAY_MS and a full remount on every one
 * of them. Revisit if that per-action cost ever proves too slow in
 * practice.
 *
 * Returns 0 on success (even if the listing was truncated to
 * max_entries), negative errno if the directory itself couldn't be
 * opened.
 */
int app_sd_list_dir(const char *path, struct app_sd_dirent *entries, size_t max_entries,
		     size_t *count);

/*
 * Prepends the SD filesystem's internal mount point to an SD-root-relative
 * path (e.g. "/books/foo.npb" -> "/SD:/books/foo.npb") - the same prefixing
 * app_sd_list_dir() does internally, exposed for a caller (reader.c) that
 * needs to fs_open()/fs_read()/fs_write() a plain file directly rather than
 * list a directory. Requires the card to already be mounted, same as
 * app_sd_list_dir(). Returns 0 on success, -ENAMETOOLONG if the result
 * doesn't fit `out_size`, -ENODEV if the card isn't mounted.
 */
int app_sd_full_path(const char *path, char *out, size_t out_size);

#endif /* APP_SD_H_ */
