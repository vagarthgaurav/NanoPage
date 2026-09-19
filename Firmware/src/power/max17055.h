/* SPDX-License-Identifier: Apache-2.0 */

#ifndef MAX17055_H_
#define MAX17055_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

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

int max17055_check_por(const struct device *dev, bool *por);

int max17055_configure(const struct device *dev, const struct max17055_learned_params *learned);

int max17055_restore_capacity(const struct device *dev,
			       const struct max17055_learned_params *learned);

int max17055_clear_por(const struct device *dev);

int max17055_read_all(const struct device *dev, struct max17055_data_snapshot *out);

int max17055_force_full(const struct device *dev);

int max17055_read_learned_params(const struct device *dev, struct max17055_learned_params *out);

#endif /* MAX17055_H_ */
