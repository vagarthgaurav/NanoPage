/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CURRENTLOG_H_
#define APP_CURRENTLOG_H_

#include <stdint.h>

/*
 * Appends one CSV row (timestamp, instantaneous current, average current,
 * voltage) to a log file on the SD card - the exact same reading the
 * footer just drew (see app_footer_get_last() in footer.h), passed in
 * rather than re-read from the fuel gauge, so the log can never disagree
 * with what's on screen.
 *
 * A no-op if the SD card isn't mounted (app_sd_full_path() returns
 * -ENODEV in that case - see sd.h) - callers don't need to check
 * themselves, same convention as app_footer_draw() failing quietly when
 * there's nothing to read.
 */
void app_currentlog_write(int32_t instant_current_ua, int32_t avg_current_ua, int64_t voltage_uv);

#endif /* APP_CURRENTLOG_H_ */
