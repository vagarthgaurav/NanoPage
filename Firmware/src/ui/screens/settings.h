/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SETTINGS_H_
#define APP_SETTINGS_H_

#include <stdbool.h>

void app_settings_init(void (*on_exit)(void));

bool app_settings_is_active(void);

#endif /* APP_SETTINGS_H_ */
