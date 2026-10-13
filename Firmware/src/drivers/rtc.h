/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_RTC_H_
#define APP_RTC_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/rtc.h>

int app_rtc_init(void);

/* Read the current RTC time. Returns 0 on success, negative errno on failure. */
int app_rtc_get_time(struct rtc_time *now);

/* Set the RTC. Uses tm_year/mon/mday/hour/min/sec; tm_wday is recomputed from
 * the date (the caller's value is ignored). The minute-tick alarm armed via
 * app_rtc_set_alarm() lives in separate registers and keeps working. Safe from
 * any thread. Returns 0 on success, -EINVAL for an out-of-range time (the RTC
 * only holds 2000..2099), or the driver's negative errno.
 */
int app_rtc_set_time(const struct rtc_time *t);

/* Minutes since 2000-01-01 00:00 on the RTC's local wall clock, into *out.
 * Returns 0 on success, negative errno if the RTC read failed or the time is
 * before 2000.
 */
int app_rtc_get_minutes_since_2000(uint32_t *out);

/* Read the current RTC time and print it via printk. */
void app_rtc_log_time(void);

/* Format the current time as "HH:MM" into buf (needs at least 6 bytes).
 * Returns 0 on success, negative errno if the RTC read failed.
 */
int app_rtc_format_time(char *buf, size_t len);

/* Format the current date as "Www, YYYY-MM-DD" into buf (needs at least 16 bytes).
 * Returns 0 on success, negative errno if the RTC read failed.
 */
int app_rtc_format_date(char *buf, size_t len);

int app_rtc_format_datetime_iso(char *buf, size_t len);

typedef void (*app_rtc_alarm_cb_t)(void);

int app_rtc_set_alarm(const struct rtc_time *time, uint16_t mask, app_rtc_alarm_cb_t cb);

/* Convenience wrapper: arm a recurring alarm that fires every day at hour:min. */
int app_rtc_set_daily_alarm(uint8_t hour, uint8_t min, app_rtc_alarm_cb_t cb);

/* Disarm whatever alarm is currently set via app_rtc_set_alarm()/app_rtc_set_daily_alarm(). */
int app_rtc_clear_alarm(void);

#endif /* APP_RTC_H_ */
