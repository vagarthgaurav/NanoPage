/* SPDX-License-Identifier: Apache-2.0 */

#define DT_DRV_COMPAT nanopage_max17055

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "i2c1_bus.h"
#include "max17055.h"

/* Register map (word registers, little-endian on the wire). */
enum {
	REG_STATUS = 0x00,
	REG_REPCAP = 0x05,
	REG_REPSOC = 0x06,
	REG_AGE = 0x07,
	REG_TEMP = 0x08,
	REG_VCELL = 0x09,
	REG_CURRENT = 0x0A,
	REG_AVGCURRENT = 0x0B,
	REG_QRESIDUAL = 0x0C,
	REG_MIXSOC = 0x0D,
	REG_MIXCAP = 0x0F,
	REG_FULLCAPREP = 0x10,
	REG_TTE = 0x11,
	REG_CYCLES = 0x17,
	REG_DESIGNCAP = 0x18,
	REG_AVGVCELL = 0x19,
	REG_ICHGTERM = 0x1E,
	REG_TTF = 0x20,
	REG_FULLCAPNOM = 0x23,
	REG_RCOMP0 = 0x38,
	REG_TEMPCO = 0x39,
	REG_VEMPTY = 0x3A,
	REG_FSTAT = 0x3D,
	REG_DQACC = 0x45,
	REG_DPACC = 0x46,
	REG_COMMAND = 0x60,
	REG_HIBCFG = 0xBA,
	REG_MODELCFG = 0xDB,
	REG_CONFIG = 0x1D,
};

#define STATUS_POR BIT(1)
#define FSTAT_DNR BIT(0)
#define MODELCFG_REFRESH BIT(15)
#define CONFIG_ETHRM BIT(4)

struct max17055_config {
	struct i2c_dt_spec i2c;
	uint16_t rsense_mohm;
	uint16_t design_capacity_mah;
	uint16_t design_voltage_mv;
	uint16_t ichg_term_ma;
	uint16_t vempty_mv;
	uint16_t vrecover_mv;
};

static int reg_read(const struct device *dev, uint8_t reg, uint16_t *val)
{
	const struct max17055_config *cfg = dev->config;
	uint8_t buf[2];
	int ret = i2c_burst_read_dt(&cfg->i2c, reg, buf, sizeof(buf));

	if (ret < 0) {
		return ret;
	}

	*val = sys_get_le16(buf);
	return 0;
}

static int reg_write(const struct device *dev, uint8_t reg, uint16_t val)
{
	const struct max17055_config *cfg = dev->config;
	uint8_t buf[3] = {reg};

	sys_put_le16(val, &buf[1]);
	return i2c_write_dt(&cfg->i2c, buf, sizeof(buf));
}

static int reg_write_verify(const struct device *dev, uint8_t reg, uint16_t val)
{
	for (int attempt = 0; attempt < 3; attempt++) {
		uint16_t readback;
		int ret = reg_write(dev, reg, val);

		if (ret) {
			return ret;
		}

		k_sleep(K_MSEC(1));

		ret = reg_read(dev, reg, &readback);
		if (ret) {
			return ret;
		}

		if (readback == val) {
			return 0;
		}
	}

	return -EIO;
}

/* Capacity registers: 5 uVh/Rsense(ohm) per LSB, i.e. mAh = raw * 5 / Rsense_mohm. */
static uint16_t mah_to_reg(const struct max17055_config *cfg, uint16_t mah)
{
	return (uint32_t)mah * cfg->rsense_mohm / 5;
}

static uint16_t reg_to_mah(const struct max17055_config *cfg, uint16_t raw)
{
	return (uint32_t)raw * 5 / cfg->rsense_mohm;
}

/* Current registers: 1.5625 uV/Rsense(ohm) per LSB, i.e. mA = raw * 25 / Rsense_mohm / 16
 * (same formula Zephyr's in-tree max17055 driver uses).
 */
static uint16_t ma_to_reg(const struct max17055_config *cfg, uint16_t ma)
{
	return (uint32_t)ma * cfg->rsense_mohm * 16 / 25;
}

static int32_t reg_to_current_ua(const struct max17055_config *cfg, int16_t raw)
{
	return (int32_t)raw * 25000 / ((int32_t)cfg->rsense_mohm * 16);
}

static int32_t reg_to_voltage_uv(uint16_t raw)
{
	/* 0.078125 mV/LSB = 5/64 mV/LSB exactly. */
	return (int32_t)raw * 625 / 8;
}

/* Percent registers (SOC, Age): 1/256 %/LSB -> tenths of a percent. */
static uint16_t reg_to_percent_x10(uint16_t raw)
{
	return (uint32_t)raw * 10 / 256;
}

int max17055_check_por(const struct device *dev, bool *por)
{
	uint16_t status;
	int ret;

	app_i2c1_lock();
	ret = reg_read(dev, REG_STATUS, &status);
	app_i2c1_unlock();

	if (ret) {
		return ret;
	}

	*por = (status & STATUS_POR) != 0;
	return 0;
}

int max17055_clear_por(const struct device *dev)
{
	uint16_t status;
	int ret;

	app_i2c1_lock();

	ret = reg_read(dev, REG_STATUS, &status);
	if (ret) {
		app_i2c1_unlock();
		return ret;
	}

	ret = reg_write_verify(dev, REG_STATUS, status & ~STATUS_POR);
	app_i2c1_unlock();

	return ret;
}

int max17055_configure(const struct device *dev, const struct max17055_learned_params *learned)
{
	const struct max17055_config *cfg = dev->config;
	uint16_t hib_cfg, fstat, model_cfg, dpacc;
	uint16_t design_cap_reg = mah_to_reg(cfg, cfg->design_capacity_mah);
	uint16_t vempty_reg = ((cfg->vempty_mv / 10) << 7) | ((cfg->vrecover_mv / 40) & 0x7F);
	int ret;

	app_i2c1_lock();

	/* Step 2: wait for FSTAT.DNR to clear (chip finishing its own startup). */
	do {
		ret = reg_read(dev, REG_FSTAT, &fstat);
		if (ret) {
			goto unlock;
		}
		if (fstat & FSTAT_DNR) {
			k_sleep(K_MSEC(10));
		}
	} while (fstat & FSTAT_DNR);

	/* Step 3: exit hibernate (save/restore HibCfg around it). */
	ret = reg_read(dev, REG_HIBCFG, &hib_cfg);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_COMMAND, 0x0090);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_HIBCFG, 0x0000);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_COMMAND, 0x0000);
	if (ret) {
		goto unlock;
	}

	uint16_t config_reg;

	ret = reg_read(dev, REG_CONFIG, &config_reg);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_CONFIG, config_reg & ~CONFIG_ETHRM);
	if (ret) {
		goto unlock;
	}

	/* Option 2: Custom Short INI without OCV table. */
	ret = reg_write(dev, REG_DESIGNCAP, design_cap_reg);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_DQACC, design_cap_reg / 16);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_ICHGTERM, ma_to_reg(cfg, cfg->ichg_term_ma));
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_VEMPTY, vempty_reg);
	if (ret) {
		goto unlock;
	}

	if (cfg->design_voltage_mv > 4275) {
		dpacc = 0x0C80;
		model_cfg = 0x8400;
	} else {
		dpacc = 0x0AC7;
		model_cfg = 0x8000;
	}
	ret = reg_write(dev, REG_DPACC, dpacc);
	if (ret) {
		goto unlock;
	}
	ret = reg_write(dev, REG_MODELCFG, model_cfg);
	if (ret) {
		goto unlock;
	}

	/* Poll ModelCfg.Refresh until the chip finishes loading the model. */
	do {
		ret = reg_read(dev, REG_MODELCFG, &model_cfg);
		if (ret) {
			goto unlock;
		}
		if (model_cfg & MODELCFG_REFRESH) {
			k_sleep(K_MSEC(10));
		}
	} while (model_cfg & MODELCFG_REFRESH);

	if (learned) {
		ret = reg_write(dev, REG_RCOMP0, learned->rcomp0);
		if (ret) {
			goto unlock;
		}
		ret = reg_write(dev, REG_TEMPCO, learned->tempco);
		if (ret) {
			goto unlock;
		}
	}

	ret = reg_write(dev, REG_HIBCFG, hib_cfg);

unlock:
	app_i2c1_unlock();
	return ret;
}

int max17055_restore_capacity(const struct device *dev,
			       const struct max17055_learned_params *learned)
{
	uint16_t mix_soc, full_cap_nom, mix_cap;
	int ret;

	if (!learned || learned->cycles == 0) {
		return -EINVAL;
	}

	app_i2c1_lock();

	/* 4.6: restore FullCapNom (RCOMP0/TempCo already set by max17055_configure()). */
	ret = reg_write_verify(dev, REG_FULLCAPNOM, learned->full_cap_nom);
	if (ret) {
		goto unlock;
	}

	k_sleep(K_MSEC(350)); /* 4.7 */

	/* 4.8: restore FullCap. */
	ret = reg_read(dev, REG_FULLCAPNOM, &full_cap_nom);
	if (ret) {
		goto unlock;
	}
	ret = reg_read(dev, REG_MIXSOC, &mix_soc);
	if (ret) {
		goto unlock;
	}
	mix_cap = (uint32_t)mix_soc * full_cap_nom / 25600;

	ret = reg_write_verify(dev, REG_MIXCAP, mix_cap);
	if (ret) {
		goto unlock;
	}
	ret = reg_write_verify(dev, REG_FULLCAPREP, learned->full_cap_rep);
	if (ret) {
		goto unlock;
	}
	ret = reg_write_verify(dev, REG_DPACC, 0x0C80);
	if (ret) {
		goto unlock;
	}
	ret = reg_write_verify(dev, REG_DQACC, learned->full_cap_nom / 16);
	if (ret) {
		goto unlock;
	}

	k_sleep(K_MSEC(350)); /* 4.10 */

	ret = reg_write_verify(dev, REG_CYCLES, learned->cycles); /* 4.11 */

unlock:
	app_i2c1_unlock();
	return ret;
}

int max17055_force_full(const struct device *dev)
{
	uint16_t rep_cap;
	int ret;

	app_i2c1_lock();

	ret = reg_read(dev, REG_REPCAP, &rep_cap);
	if (ret) {
		app_i2c1_unlock();
		return ret;
	}

	ret = reg_write_verify(dev, REG_FULLCAPREP, rep_cap);
	app_i2c1_unlock();

	return ret;
}

int max17055_read_learned_params(const struct device *dev, struct max17055_learned_params *out)
{
	int ret;

	app_i2c1_lock();

	ret = reg_read(dev, REG_RCOMP0, &out->rcomp0);
	if (ret) {
		goto unlock;
	}
	ret = reg_read(dev, REG_TEMPCO, &out->tempco);
	if (ret) {
		goto unlock;
	}
	ret = reg_read(dev, REG_FULLCAPREP, &out->full_cap_rep);
	if (ret) {
		goto unlock;
	}
	ret = reg_read(dev, REG_FULLCAPNOM, &out->full_cap_nom);
	if (ret) {
		goto unlock;
	}
	ret = reg_read(dev, REG_CYCLES, &out->cycles);

unlock:
	app_i2c1_unlock();
	return ret;
}

int max17055_read_all(const struct device *dev, struct max17055_data_snapshot *out)
{
	const struct max17055_config *cfg = dev->config;
	uint16_t raw;
	int ret;

#define READ(reg_addr, dest)                                                                     \
	do {                                                                                       \
		ret = reg_read(dev, (reg_addr), &raw);                                            \
		if (ret) {                                                                        \
			app_i2c1_unlock();                                                        \
			return ret;                                                               \
		}                                                                                 \
		(dest);                                                                           \
	} while (0)

	app_i2c1_lock();

	READ(REG_VCELL, out->voltage_uv = reg_to_voltage_uv(raw));
	READ(REG_AVGVCELL, out->avg_voltage_uv = reg_to_voltage_uv(raw));
	READ(REG_CURRENT, out->current_ua = reg_to_current_ua(cfg, (int16_t)raw));
	READ(REG_AVGCURRENT, out->avg_current_ua = reg_to_current_ua(cfg, (int16_t)raw));
	READ(REG_REPSOC, out->soc_x10 = reg_to_percent_x10(raw));
	READ(REG_AGE, out->age_x10 = reg_to_percent_x10(raw));
	READ(REG_REPCAP, out->rep_cap_mah = reg_to_mah(cfg, raw));
	READ(REG_FULLCAPREP, out->full_cap_rep_mah = reg_to_mah(cfg, raw));
	READ(REG_FULLCAPNOM, out->full_cap_nom_mah = reg_to_mah(cfg, raw));
	READ(REG_DESIGNCAP, out->design_cap_mah = reg_to_mah(cfg, raw));
	READ(REG_QRESIDUAL, out->qresidual_mah = reg_to_mah(cfg, raw));
	READ(REG_TEMP, out->temp_c_x10 = (int32_t)(int16_t)raw * 10 / 256);
	READ(REG_TTE, out->tte_sec = (uint32_t)raw * 45 / 8);
	READ(REG_TTF, out->ttf_sec = (uint32_t)raw * 45 / 8);
	READ(REG_CYCLES, out->cycles_x100 = raw);
	READ(REG_RCOMP0, out->rcomp0 = raw);
	READ(REG_TEMPCO, out->tempco = raw);

#undef READ

	app_i2c1_unlock();
	return 0;
}

static int max17055_init(const struct device *dev)
{
	const struct max17055_config *cfg = dev->config;

	if (!device_is_ready(cfg->i2c.bus)) {
		return -ENODEV;
	}

	return 0;
}

#define MAX17055_INIT(inst)                                                                       \
	static const struct max17055_config max17055_config_##inst = {                           \
		.i2c = I2C_DT_SPEC_INST_GET(inst),                                                \
		.rsense_mohm = DT_INST_PROP(inst, rsense_mohms),                                  \
		.design_capacity_mah = DT_INST_PROP(inst, design_capacity),                       \
		.design_voltage_mv = DT_INST_PROP(inst, design_voltage),                          \
		.ichg_term_ma = DT_INST_PROP(inst, i_chg_term),                                   \
		.vempty_mv = DT_INST_PROP(inst, v_empty),                                         \
		.vrecover_mv = DT_INST_PROP(inst, v_recover),                                     \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(inst, max17055_init, NULL, NULL, &max17055_config_##inst,          \
			       POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(MAX17055_INIT)
