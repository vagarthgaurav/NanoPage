/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_BOOKMARKS_H_
#define APP_BOOKMARKS_H_

#include <stdbool.h>

void app_bookmarks_show(void (*on_back)(void), void (*on_selected)(void));

bool app_bookmarks_is_active(void);

#endif /* APP_BOOKMARKS_H_ */
