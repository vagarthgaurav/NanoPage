/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CONFIG_H_
#define APP_CONFIG_H_

/* --- Power (power.c) --- */

/* Idle time (no button press) before the board drops to System OFF. */
#define APP_POWER_IDLE_TIMEOUT_MS (5 * 60 * 1000)

/* --- Status bar (main.c) --- */

#define APP_STATUS_REFRESH_INTERVAL_SEC 30

#define APP_STATUS_CURRENT_SAMPLE_LEAD_SEC 10

/* --- File browser (browser.c) --- */

#define APP_BROWSER_FULL_REFRESH_EVERY 10

/* --- Book reader (reader.c) --- */

#define APP_READER_FULL_REFRESH_EVERY 10

/* How long the SD card stays mounted/powered after the last reader action
 * before it's torn back down, so a burst of quick page turns shares one
 * mount instead of paying the mount/power-up cost per turn.
 */
#define APP_READER_SD_IDLE_MS 2000

/* --- Chapter list (toc.c) --- */

#define APP_TOC_FULL_REFRESH_EVERY 10

/* --- Bookmarks list (bookmarks.c) --- */

#define APP_BOOKMARKS_FULL_REFRESH_EVERY 10

/* --- SD card (sd.c) --- */

#define APP_SD_POWER_UP_DELAY_MS 20

/* --- Fuel gauge / charging (fuel_gauge.c) --- */

#define APP_CHARGE_COMPLETE_VOLTAGE_MV 4100

#define APP_CHARGE_FULL_DISPLAY_MARGIN_X10 30

#endif /* APP_CONFIG_H_ */
