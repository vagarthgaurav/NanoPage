/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CHARGER_H_
#define APP_CHARGER_H_

#include <stdbool.h>

int app_charger_init(void);

bool app_charger_is_charging(void);

void app_charger_set_change_callback(void (*cb)(void));

#endif /* APP_CHARGER_H_ */
