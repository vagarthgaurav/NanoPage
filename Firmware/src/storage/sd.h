/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SD_H_
#define APP_SD_H_

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

#endif /* APP_SD_H_ */
