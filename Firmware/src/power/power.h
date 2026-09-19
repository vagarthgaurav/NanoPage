/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#include <stdbool.h>

void app_power_init(void);

bool app_power_woke_from_system_off(void);

void app_power_mark_activity(void);

void app_power_set_sleep_inhibited(bool inhibited);

void app_power_set_idle_hook(void (*cb)(void));

#endif
