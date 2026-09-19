/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_READER_MENU_H_
#define APP_READER_MENU_H_

#include <stdbool.h>

void app_reader_menu_show(void (*on_exit)(void));

bool app_reader_menu_is_active(void);

#endif /* APP_READER_MENU_H_ */
