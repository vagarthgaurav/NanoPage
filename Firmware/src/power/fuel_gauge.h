/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_FUEL_GAUGE_H_
#define APP_FUEL_GAUGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "max17055.h"

/*
 * App-level policy layer on top of max17055.c (the mechanism: register
 * I/O, no automatic configuration). This owns the actual decisions:
 *
 *  - On a POR (battery disconnected/reinserted, or first-ever power-up),
 *    which impedance-track parameters to load - previously-saved ones
 *    from flash if this exact device has them, otherwise the compiled-in
 *    factory defaults (see FACTORY_* in fuel_gauge.c) learned by cycling
 *    the bench unit's battery, since every board ships with the same
 *    battery model and those are a better starting point than the chip's
 *    own from-scratch characterization.
 *  - Persisting freshly-learned parameters back to flash periodically, so
 *    they survive the next battery disconnect.
 */

/* Bring up the fuel gauge: mount the NVS storage partition, then check
 * STATUS.POR. If clear, the chip is already configured (a warm MCU reset,
 * not a battery disconnect) and this is a no-op. If set, restores
 * previously-saved learned parameters from flash when present, otherwise
 * falls back to the compiled-in factory defaults (or, if neither exists
 * yet, lets the chip characterize from scratch), then clears POR.
 * Returns 0 on success, negative errno on failure.
 */
int app_fuel_gauge_init(void);

/* Read every register this module knows how to interpret. See
 * struct max17055_data_snapshot in max17055.h for the field list - this
 * is the full detail a battery settings page would want.
 * Returns 0 on success, negative errno on I2C failure.
 */
int app_fuel_gauge_read(struct max17055_data_snapshot *out);

/* Convenience accessors for the common case (status bar). Each does its
 * own I2C reads - prefer app_fuel_gauge_read() if you need more than one
 * at a time.
 */
int app_fuel_gauge_get_percent(uint8_t *percent);
int app_fuel_gauge_get_voltage_mv(uint16_t *mv);
int app_fuel_gauge_get_current_ma(int16_t *ma);

/* True roughly once per 64% capacity change (Cycles register bit 6
 * toggling - the cadence Maxim's guide recommends for saving learned
 * parameters, to avoid wearing the flash on every reading). Call from the
 * main loop and follow up with app_fuel_gauge_save_learned_params() when
 * this returns true. Returns false (without I2C traffic) if the fuel
 * gauge hasn't been initialized yet.
 */
bool app_fuel_gauge_should_save_learned_params(void);

/* Save RCOMP0/TempCo/FullCapRep/Cycles/FullCapNom to the flash storage
 * partition, so app_fuel_gauge_init() can restore them after the next
 * POR. nvs_write() itself is a no-op if the data is unchanged, so calling
 * this more often than necessary doesn't wear the flash.
 * Returns 0 on success, negative errno on I2C or flash failure.
 */
int app_fuel_gauge_save_learned_params(void);

#endif /* APP_FUEL_GAUGE_H_ */
