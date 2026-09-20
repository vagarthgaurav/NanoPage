/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#include <stdbool.h>

void app_power_init(void);

bool app_power_woke_from_system_off(void);

void app_power_mark_activity(void);

/* Cancels the idle-timeout work and blocks until any in-flight run of it
 * (e.g. a System OFF entry already underway) has finished. Callers that are
 * about to mutate the same state the idle-timeout path touches (via its
 * idle hook) must call this first, since app_power_mark_activity()'s
 * reschedule cannot stop a run that has already started.
 */
void app_power_cancel_idle_sync(void);

void app_power_set_sleep_inhibited(bool inhibited);

void app_power_set_idle_hook(void (*cb)(void));

#endif
