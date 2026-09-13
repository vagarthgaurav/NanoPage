/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_CHARGER_H_
#define APP_CHARGER_H_

#include <stdbool.h>

/* Configure the BQ25170 STAT input (P0.03, zephyr,user/charger-status-gpios
 * in nanopage.dts). Returns 0 on success, negative errno on failure.
 */
int app_charger_init(void);

/* True while the BQ25170 reports an active charge cycle (STAT driven low).
 * See the fault-blink caveat in charger.c - this is a plain instantaneous
 * level read, not a debounced/latched charge-state machine.
 */
bool app_charger_is_charging(void);

/*
 * Registers `cb` to be called (from the system workqueue, not ISR context
 * - see the GPIO ISR -> k_work hop in charger.c) every time STAT changes
 * level, i.e. on every charger plug/unplug/complete transition. Only one
 * callback is kept; a second call replaces the first. Pass NULL to stop
 * being notified. app_charger_init() must succeed first.
 */
void app_charger_set_change_callback(void (*cb)(void));

#endif /* APP_CHARGER_H_ */
