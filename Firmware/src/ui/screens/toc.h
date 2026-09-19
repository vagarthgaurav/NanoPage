/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_TOC_H_
#define APP_TOC_H_

#include <stdbool.h>

void app_toc_show(void (*on_back)(void), void (*on_selected)(void));

bool app_toc_is_active(void);

#endif /* APP_TOC_H_ */
