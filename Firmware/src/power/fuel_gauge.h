/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FUEL_GAUGE_H_
#define APP_FUEL_GAUGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "max17055.h"

int app_fuel_gauge_init(void);

int app_fuel_gauge_read(struct max17055_data_snapshot *out);

int app_fuel_gauge_read_current_ua(int32_t *current_ua);
int app_fuel_gauge_read_percent_x10(uint16_t *soc_x10);

int app_fuel_gauge_get_percent(uint8_t *percent);
int app_fuel_gauge_get_voltage_mv(uint16_t *mv);
int app_fuel_gauge_get_current_ma(int16_t *ma);

bool app_fuel_gauge_should_save_learned_params(void);

int app_fuel_gauge_save_learned_params(void);

void app_fuel_gauge_notify_charger_state(bool charging);

#endif /* APP_FUEL_GAUGE_H_ */
