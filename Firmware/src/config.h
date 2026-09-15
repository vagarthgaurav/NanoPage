/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Central home for this firmware's tunable policy knobs - timers,
 * intervals, and thresholds that don't change behavior structurally
 * (unlike buffer sizes, layout columns, or register constants, which stay
 * local to the file that owns that structure). Collecting them here means
 * a battery-life/UX tuning pass touches one file instead of hunting
 * through every subsystem.
 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* --- Power (power.c) --- */

/* Idle time (no button press) before the board drops to System OFF. */
#define APP_POWER_IDLE_TIMEOUT_MS (5 * 60 * 1000)

/* --- Status bar (main.c) --- */

/*
 * Footer refresh cadence: partial refresh is cheap (text.c only pushes
 * the one changed row), but even a sub-second SSD1680 "fast" waveform
 * update isn't free against the battery-life goal for a value (SOC/
 * voltage) that barely moves second to second. 30s is a placeholder -
 * revisit once there's real battery-life data to tune against.
 */
#define APP_STATUS_REFRESH_INTERVAL_SEC 30

/*
 * How long before the refresh to sample the instantaneous Current
 * register - see app_footer_sample_current() in footer.h for why this
 * needs to happen ahead of the draw rather than at draw time.
 */
#define APP_STATUS_CURRENT_SAMPLE_LEAD_SEC 10

/* --- File browser (browser.c) --- */

/* Forces a full-panel refresh every this-many partial-refresh-triggering
 * actions (cursor moves and page scrolls - NOT folder navigation, which
 * always gets its own full refresh regardless of this counter) to clear
 * ghosting - see browser.h's top comment.
 */
#define APP_BROWSER_FULL_REFRESH_EVERY 10

/* --- Book reader (reader.c) --- */

/* See APP_BROWSER_FULL_REFRESH_EVERY above for the identical reasoning -
 * scoped separately rather than shared, same as header.c/footer.c/
 * browser.c each owning their own refresh decisions.
 */
#define APP_READER_FULL_REFRESH_EVERY 10

/* --- SD card (sd.c) --- */

/* Settle time after powering the SD rail on before the card is trusted to
 * respond - see the SD/display backfeed writeup at the top of sd.h.
 */
#define APP_SD_POWER_UP_DELAY_MS 20

/* --- Fuel gauge / charging (fuel_gauge.c) --- */

/* Voltage (mV) at/above which a STAT-confirmed charge completion forces
 * the displayed SOC to 100%, rather than waiting on the MAX17055's own
 * (slower-converging) RepSOC estimate.
 */
#define APP_CHARGE_COMPLETE_VOLTAGE_MV 4100

/* Displayed-SOC margin (in tenths of a percent) below 100% that still
 * counts as "full enough" once a STAT-confirmed charge completion has
 * been seen, to avoid flickering just under 100% while RepSOC settles.
 */
#define APP_CHARGE_FULL_DISPLAY_MARGIN_X10 30

#endif /* APP_CONFIG_H_ */
