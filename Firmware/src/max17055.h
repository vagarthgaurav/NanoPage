/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MAX17055_H_
#define MAX17055_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

/*
 * From-scratch MAX17055 driver (register I/O + devicetree instantiation
 * only - no automatic chip configuration). See the top-of-file comment in
 * max17055.c for why this isn't Zephyr's in-tree "maxim,max17055" driver
 * wrapped in a thin app layer: that driver's init() runs automatically at
 * POST_KERNEL, before main(), and unconditionally does a basic EZ-config
 * and clears STATUS.POR itself on every POR - there is no way to make it
 * check flash for previously-learned parameters first.
 *
 * This driver's own init() (wired via DEVICE_DT_INST_DEFINE) does nothing
 * but verify the I2C bus is ready - it never touches chip registers. All
 * actual configuration happens through the functions below, called
 * explicitly by app code (fuel_gauge.c) after main() starts, once it has
 * had a chance to read NVS. That split is what makes the restore-on-POR
 * behavior possible at all - see the design-taste discussion this was
 * built from in .claude/CLAUDE.md.
 */

/*
 * The subset of chip state Maxim's software implementation guide (docs/
 * max17055-software-implementation-guide.pdf, "4.5 Save learned
 * parameters" / "4.6-4.11 Restoring capacity parameters") persists across
 * a POR. Fields are raw register values, not engineering units - the
 * datasheet procedure is a pure register passthrough (read these 5
 * registers, write them back later), and keeping them raw avoids any
 * conversion/rounding loss on the round trip. Treat as opaque outside of
 * max17055_configure()/max17055_restore_capacity() and whatever storage
 * fuel_gauge.c persists them in.
 */
struct max17055_learned_params {
	uint16_t rcomp0;       /* RCOMP0 (0x38) - impedance-track parameter */
	uint16_t tempco;       /* TempCo (0x39) - impedance temperature coefficient */
	uint16_t full_cap_rep; /* FullCapRep (0x10) - learned full capacity */
	uint16_t full_cap_nom; /* FullCapNom (0x23) - nominal (unaged) full capacity */
	uint16_t cycles;       /* Cycles (0x17) - charge/discharge cycle count, 1/100 units */
};

/* Full register snapshot, already converted to engineering units - for a
 * status bar or a future battery settings page. */
struct max17055_data_snapshot {
	int32_t voltage_uv;     /* VCell: instantaneous cell voltage */
	int32_t avg_voltage_uv; /* AvgVCell */
	int32_t current_ua;     /* Current: +ve = charging, -ve = discharging */
	int32_t avg_current_ua; /* AvgCurrent */
	uint16_t soc_x10;       /* RepSOC, tenths of a percent (e.g. 723 = 72.3%) */
	uint16_t age_x10;       /* Age, tenths of a percent (FullCapNom / DesignCap) */
	uint16_t rep_cap_mah;   /* RepCap: reported remaining capacity */
	uint16_t full_cap_rep_mah;
	uint16_t full_cap_nom_mah;
	uint16_t design_cap_mah;
	uint16_t qresidual_mah;
	int16_t temp_c_x10;  /* Temp: die temperature (no external NTC on this board), tenths of degC */
	uint32_t tte_sec;    /* Time to empty at the present discharge rate */
	uint32_t ttf_sec;    /* Time to full at the present charge rate */
	uint16_t cycles_x100; /* Cycles, raw (hundredths of a full charge/discharge cycle) */
	uint16_t rcomp0;      /* opaque - exposed for a settings-page raw dump only */
	uint16_t tempco;      /* opaque - exposed for a settings-page raw dump only */
};

/* True if STATUS.POR is set: the chip has lost its own supply since the
 * bit was last cleared (battery disconnect/reinsertion, or first-ever
 * power-up) - the only reset the chip's internal impedance-track model
 * doesn't survive. Cleared only by max17055_clear_por().
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_check_por(const struct device *dev, bool *por);

/*
 * Run Maxim's "Custom Short INI without OCV table" sequence (Software
 * Implementation Guide section 3, option 2): waits for FSTAT.DNR, exits
 * hibernate, writes DesignCap/dQAcc/IChgTerm/VEmpty/dPAcc/ModelCfg from
 * devicetree, polls ModelCfg.Refresh, then writes RCOMP0/TempCo from
 * `learned` if non-NULL (pass NULL to skip those two writes entirely and
 * let the chip's own algorithm characterize from scratch - the right
 * choice when no learned/factory data exists yet, since writing literal
 * zeros would override the chip's own POR defaults with wrong data
 * instead of just leaving them alone).
 *
 * Blocks synchronously (the ModelCfg.Refresh poll can take up to ~1s).
 * Does not touch STATUS.POR - call max17055_clear_por() once the whole
 * sequence (including max17055_restore_capacity(), if used) succeeds.
 *
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_configure(const struct device *dev, const struct max17055_learned_params *learned);

/*
 * Restore capacity/cycle-count state on top of an already-configured chip
 * - the FullCapRep/FullCapNom/MixCap/Cycles half of the datasheet's
 * save/restore procedure (section 4.6-4.11), run after max17055_configure()
 * when `learned` came from a real save (not a placeholder). RCOMP0/TempCo
 * are deliberately NOT rewritten here even though the datasheet's own 4.6
 * step includes them - max17055_configure() above already wrote them from
 * the same `learned` struct, so redoing it here would just be a same-value
 * no-op write.
 *
 * Involves two blocking 350ms delays per the datasheet procedure. Do not
 * call with a `learned` struct whose cycles field is 0 - that means "no
 * real capacity history", and this would incorrectly seed a full/nominal
 * capacity of 0.
 *
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_restore_capacity(const struct device *dev,
			       const struct max17055_learned_params *learned);

/* Clear STATUS.POR. Call once configuration (and capacity restore, if
 * any) has completed successfully - this is what tells the chip (and the
 * next boot) that it doesn't need to be reconfigured again.
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_clear_por(const struct device *dev);

/* Read every register this driver knows how to interpret into *out.
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_read_all(const struct device *dev, struct max17055_data_snapshot *out);

/* Read just the fields struct max17055_learned_params needs - cheaper
 * than max17055_read_all() when that's all the caller needs (the periodic
 * save-to-flash path).
 * Returns 0 on success, negative errno on I2C failure.
 */
int max17055_read_learned_params(const struct device *dev, struct max17055_learned_params *out);

#endif /* MAX17055_H_ */
