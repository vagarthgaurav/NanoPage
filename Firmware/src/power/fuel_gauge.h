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

/*
 * Feed the charger's STAT-derived charging state (see charger.c's
 * app_charger_is_charging()) in here on every change - harmless to also
 * call it on unrelated ticks (e.g. the RTC minute alarm), since it only
 * acts on an actual charging->not-charging transition.
 *
 * Works around a real MAX17055 limitation, not a bug in this board's I2C
 * config: the chip's own automatic full detection (RepSOC -> 100%,
 * FullCapRep <- RepCap) only fires once RepSOC has already crossed the
 * FullSOCthr gate, left at its POR default (~95%) since this board's
 * "Option 2: Custom Short INI" max17055_configure() sequence never writes
 * it (docs/max17055-software-implementation-guide.pdf only writes
 * FullSOCthr under Option 3's Custom Full INI). On a battery whose
 * impedance-track model hasn't converged yet - e.g. the very first real
 * charge cycle, characterizing from scratch - RepSOC can plateau below
 * that 95% gate even once the charger has genuinely finished, so the
 * automatic correction never fires and the display sticks at whatever
 * RepSOC happened to reach (e.g. 68%).
 *
 * Fixed by using the BQ25170's STAT pin as ground truth instead: on a
 * charging->not-charging transition, if cell voltage confirms the CV-phase
 * taper actually completed (as opposed to USB simply being unplugged
 * mid-charge - both cause the identical STAT transition, see charger.c's
 * own comment), this manually performs the same FullCapRep <- RepCap
 * update the chip's own full detection would have done.
 */
void app_fuel_gauge_notify_charger_state(bool charging);

#endif /* APP_FUEL_GAUGE_H_ */
