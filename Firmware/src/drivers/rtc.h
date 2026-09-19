/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_RTC_H_
#define APP_RTC_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/drivers/rtc.h>

int app_rtc_init(void);

/* Read the current RTC time. Returns 0 on success, negative errno on failure. */
int app_rtc_get_time(struct rtc_time *now);

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
