/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CALENDAR_DAY_H_
#define APP_CALENDAR_DAY_H_

#include <stdbool.h>
#include <stddef.h>

void app_calendar_day_show(void (*on_exit)(void));

bool app_calendar_day_is_active(void);

/* Draws today's day view into the text framebuffer, without cursor, button
 * callbacks or any flush/refresh. The caller flushes. Does not mark the view active.
 */
void app_calendar_day_draw_snapshot(void);

/* Format the viewed day as "Www, MM-DD-YY" into buf (needs at least 14 bytes).
 * Returns 0 on success, negative errno on failure.
 */
int app_calendar_day_format_date(char *buf, size_t len);

#endif /* APP_CALENDAR_DAY_H_ */
