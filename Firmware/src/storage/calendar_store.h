/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Calendar event store: the blob the phone pushes over the companion link
 * (format in companion_proto.h, "Calendar blob"), kept on the SD card as
 * /.nanopage/calendar.bin and read back by the day view.
 *
 * Times are minutes since 2000-01-01 00:00 local wall clock. An event belongs
 * to the day its start_local falls on; the phone splits multi-day events.
 */

#ifndef APP_CALENDAR_STORE_H_
#define APP_CALENDAR_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest blob accepted, in bytes. 500 events at 8 bytes + a title of up to
 * 47 characters is ~27 KB worst case, ~16 KB with typical titles.
 */
#define APP_CALENDAR_STORE_MAX_BLOB 24576

#define APP_CALENDAR_STORE_MAX_EVENTS 500

/* Characters of a title the day view can ever show; longer ones are cut. */
#define APP_CALENDAR_STORE_TITLE_MAX 47

#define APP_CALENDAR_STORE_MINUTES_PER_DAY 1440

/* Days from 1970-01-01 to 2000-01-01. */
#define APP_CALENDAR_STORE_EPOCH_OFFSET_DAYS 10957

struct app_calendar_store_event {
	uint16_t start_min; /* minute of day, 0 for all-day events */
	uint16_t dur_min;   /* 0xFFFF for all-day events */
	bool all_day;
	char title[APP_CALENDAR_STORE_TITLE_MAX + 1]; /* NUL terminated, cut to fit */
};

/* Pure structural check, no I/O: magic "NPCL", version 1, header event_count
 * equal to `expected_count`, event_count <= MAX_EVENTS, exactly event_count
 * events that consume exactly `len` bytes, every title 7-bit ASCII 32..126.
 * Returns 0 or -EINVAL. Used both when receiving and when loading, so a blob
 * cut short by a power loss or edited by hand is never accepted.
 */
int app_calendar_store_validate(const uint8_t *blob, size_t len, uint16_t expected_count);

/* Stores an already validated blob: SD up, /.nanopage created if missing,
 * written to calendar.tmp and synced, then the old calendar.bin removed and
 * the tmp renamed over it (FatFs cannot rename onto an existing file). If
 * power is lost between the unlink and the rename, load() finds the complete
 * calendar.tmp. Returns 0, -EBUSY if the card is not available (USB mass
 * storage owns it), or another negative errno. On any error the previous
 * calendar.bin is left untouched.
 */
int app_calendar_store_save(const uint8_t *blob, size_t len);

/* Reads the stored calendar into the store's own RAM buffer (replacing what
 * was loaded) and validates it. Brings the SD card up and down itself. Falls
 * back to calendar.tmp if calendar.bin is missing. Returns the number of
 * events (>= 0), -ENOENT if nothing is stored, -EINVAL if the file is corrupt
 * (nothing stays loaded), -EBUSY if the card is not available, or another
 * negative errno. Not safe to call concurrently with itself.
 */
int app_calendar_store_load(void);

/* Events of one local day from the loaded calendar. `days_since_2000` is the
 * day number counted from 2000-01-01 (= days since 1970-01-01 minus
 * APP_CALENDAR_STORE_EPOCH_OFFSET_DAYS). All-day events come first, then
 * timed ones by start time (stable). Fills at most `max` entries and returns
 * how many; 0 if nothing is loaded.
 */
size_t app_calendar_store_get_day(int32_t days_since_2000, struct app_calendar_store_event *out,
				  size_t max);

#endif /* APP_CALENDAR_STORE_H_ */
