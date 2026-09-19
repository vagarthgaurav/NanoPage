/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BROWSER_H_
#define APP_BROWSER_H_

#include <stdbool.h>

int app_browser_init(const char *root, void (*on_exit)(void));

int app_browser_reinit(void);

bool app_browser_is_ready(void);

const char *app_browser_get_current_path(void);

#endif /* APP_BROWSER_H_ */
