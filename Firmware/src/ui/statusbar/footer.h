/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FOOTER_H_
#define APP_FOOTER_H_

#include <stdint.h>

void app_footer_init(void);

void app_footer_sample_current(void);

int app_footer_draw(void);

struct app_footer_reading {
	int32_t instant_current_ua;
	int32_t avg_current_ua;
	int64_t voltage_uv;
	uint16_t soc_x10;
};

void app_footer_get_last(struct app_footer_reading *out);

#endif /* APP_FOOTER_H_ */
