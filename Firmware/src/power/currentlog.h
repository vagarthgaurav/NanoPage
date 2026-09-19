/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CURRENTLOG_H_
#define APP_CURRENTLOG_H_

#include <stdint.h>

void app_currentlog_write(int32_t instant_current_ua, int32_t avg_current_ua, int64_t voltage_uv);

#endif /* APP_CURRENTLOG_H_ */
