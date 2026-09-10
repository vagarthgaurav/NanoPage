/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_RTC_H_
#define APP_RTC_H_

/* Bring up the RTC device and seed it with a fixed reference time.
 * Returns 0 on success, negative errno on failure.
 */
int app_rtc_init(void);

/* Read the current RTC time and print it via printk. */
void app_rtc_log_time(void);

#endif /* APP_RTC_H_ */
