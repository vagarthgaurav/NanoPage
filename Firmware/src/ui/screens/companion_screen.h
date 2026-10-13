/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_COMPANION_SCREEN_H_
#define APP_COMPANION_SCREEN_H_

#include <stdbool.h>

void app_companion_screen_show(void (*on_exit)(void));

bool app_companion_screen_is_active(void);

#endif /* APP_COMPANION_SCREEN_H_ */
