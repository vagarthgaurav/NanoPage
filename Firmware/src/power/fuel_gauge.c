/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>

#include "config.h"
#include "fuel_gauge.h"
#include "max17055.h"

static const struct device *const fg_dev = DEVICE_DT_GET(DT_NODELABEL(fuel_gauge_ext));

#define NVS_PARTITION storage_partition
#define NVS_PARTITION_DEVICE PARTITION_DEVICE(NVS_PARTITION)
#define NVS_PARTITION_OFFSET PARTITION_OFFSET(NVS_PARTITION)
#define NVS_ID_LEARNED_PARAMS 1

static struct nvs_fs nvs;
static bool nvs_ready;

#define FACTORY_VALID 0
static const struct max17055_learned_params factory_defaults = {
	.rcomp0 = 0,
	.tempco = 0,
	.full_cap_rep = 0,
	.full_cap_nom = 0,
	.cycles = 0,
};

static int nvs_init_once(void)
{
	struct flash_pages_info info;
	int ret;

	nvs.flash_device = NVS_PARTITION_DEVICE;
	if (!device_is_ready(nvs.flash_device)) {
		printk("fuel_gauge: storage partition flash device not ready\n");
		return -ENODEV;
	}

	nvs.offset = NVS_PARTITION_OFFSET;
	ret = flash_get_page_info_by_offs(nvs.flash_device, nvs.offset, &info);
	if (ret) {
		printk("fuel_gauge: flash_get_page_info_by_offs failed: %d\n", ret);
		return ret;
	}

	nvs.sector_size = info.size;
	nvs.sector_count = PARTITION_SIZE(NVS_PARTITION) / info.size;

	ret = nvs_mount(&nvs);
	if (ret) {
		printk("fuel_gauge: nvs_mount failed: %d\n", ret);
		return ret;
	}

	nvs_ready = true;
	return 0;
}

static bool nvs_load_learned_params(struct max17055_learned_params *out)
{
	ssize_t ret;

	if (!nvs_ready) {
		return false;
	}

	ret = nvs_read(&nvs, NVS_ID_LEARNED_PARAMS, out, sizeof(*out));
	return ret == sizeof(*out);
}

int app_fuel_gauge_init(void)
{
	struct max17055_learned_params saved;
	const struct max17055_learned_params *to_apply = NULL;
	bool por;
	bool have_capacity_history = false;
	int ret;

	if (!device_is_ready(fg_dev)) {
		printk("fuel_gauge: device not ready\n");
		return -ENODEV;
	}

	(void)nvs_init_once();

	ret = max17055_check_por(fg_dev, &por);
	if (ret) {
		printk("fuel_gauge: STATUS read failed: %d\n", ret);
		return ret;
	}

	if (!por) {
		printk("fuel_gauge: no POR - already configured\n");
	} else {
		if (nvs_load_learned_params(&saved)) {
			printk("fuel_gauge: POR - restoring learned params from flash "
			       "(rcomp0=0x%04x tempco=0x%04x fullcaprep=%u fullcapnom=%u "
			       "cycles=%u)\n",
			       saved.rcomp0, saved.tempco, saved.full_cap_rep, saved.full_cap_nom,
			       saved.cycles);
			to_apply = &saved;
			have_capacity_history = (saved.cycles != 0);
		} else if (FACTORY_VALID) {
			printk("fuel_gauge: POR - no saved data, using factory defaults\n");
			to_apply = &factory_defaults;
			have_capacity_history = (factory_defaults.cycles != 0);
		} else {
			printk("fuel_gauge: POR - no saved or factory data, characterizing from "
			       "scratch\n");
		}

		ret = max17055_configure(fg_dev, to_apply);
		if (ret) {
			printk("fuel_gauge: configure failed: %d\n", ret);
			return ret;
		}

		if (have_capacity_history) {
			ret = max17055_restore_capacity(fg_dev, to_apply);
			if (ret) {
				printk("fuel_gauge: restore_capacity failed: %d\n", ret);
				return ret;
			}
		}

		ret = max17055_clear_por(fg_dev);
		if (ret) {
			printk("fuel_gauge: clear POR failed: %d\n", ret);
			return ret;
		}
	}

	struct max17055_learned_params now;

	if (max17055_read_learned_params(fg_dev, &now) == 0) {
		printk("fuel_gauge: learned params - rcomp0=0x%04x tempco=0x%04x fullcaprep=%u "
		       "fullcapnom=%u cycles=%u\n",
		       now.rcomp0, now.tempco, now.full_cap_rep, now.full_cap_nom, now.cycles);
	}

	return 0;
}

static bool charge_full_latched;

static void apply_full_charge_latch(struct max17055_data_snapshot *snap)
{
	if (!charge_full_latched) {
		return;
	}

	if (snap->soc_x10 < (1000 - APP_CHARGE_FULL_DISPLAY_MARGIN_X10)) {
		charge_full_latched = false;
		return;
	}

	snap->soc_x10 = 1000;
}

int app_fuel_gauge_read(struct max17055_data_snapshot *out)
{
	int ret = max17055_read_all(fg_dev, out);

	if (ret == 0) {
		apply_full_charge_latch(out);
	}

	return ret;
}

int app_fuel_gauge_read_current_ua(int32_t *current_ua)
{
	return max17055_read_current(fg_dev, current_ua);
}

int app_fuel_gauge_read_percent_x10(uint16_t *soc_x10)
{
	struct max17055_data_snapshot snap = {0};
	int ret = max17055_read_soc(fg_dev, &snap.soc_x10);

	if (ret) {
		return ret;
	}

	apply_full_charge_latch(&snap);
	*soc_x10 = snap.soc_x10;
	return 0;
}

int app_fuel_gauge_get_percent(uint8_t *percent)
{
	struct max17055_data_snapshot snap;
	int ret = app_fuel_gauge_read(&snap);

	if (ret) {
		return ret;
	}

	*percent = (uint8_t)(snap.soc_x10 / 10);
	return 0;
}

int app_fuel_gauge_get_voltage_mv(uint16_t *mv)
{
	struct max17055_data_snapshot snap;
	int ret = max17055_read_all(fg_dev, &snap);

	if (ret) {
		return ret;
	}

	*mv = (uint16_t)(snap.voltage_uv / 1000);
	return 0;
}

int app_fuel_gauge_get_current_ma(int16_t *ma)
{
	struct max17055_data_snapshot snap;
	int ret = max17055_read_all(fg_dev, &snap);

	if (ret) {
		return ret;
	}

	*ma = (int16_t)(snap.current_ua / 1000);
	return 0;
}

bool app_fuel_gauge_should_save_learned_params(void)
{
	static bool last_bit6;
	static bool have_last;
	struct max17055_learned_params cur;
	bool bit6;

	if (!device_is_ready(fg_dev)) {
		return false;
	}

	if (max17055_read_learned_params(fg_dev, &cur)) {
		return false;
	}

	bit6 = (cur.cycles & BIT(6)) != 0;

	if (!have_last) {
		last_bit6 = bit6;
		have_last = true;
		return false;
	}

	if (bit6 != last_bit6) {
		last_bit6 = bit6;
		return true;
	}

	return false;
}

int app_fuel_gauge_save_learned_params(void)
{
	struct max17055_learned_params cur;
	int ret = max17055_read_learned_params(fg_dev, &cur);

	if (ret) {
		return ret;
	}

	if (!nvs_ready) {
		return -ENODEV;
	}

	ret = nvs_write(&nvs, NVS_ID_LEARNED_PARAMS, &cur, sizeof(cur));
	if (ret < 0) {
		printk("fuel_gauge: nvs_write failed: %d\n", ret);
		return ret;
	}

	printk("fuel_gauge: saved learned params (rcomp0=0x%04x tempco=0x%04x fullcaprep=%u "
	       "fullcapnom=%u cycles=%u)\n",
	       cur.rcomp0, cur.tempco, cur.full_cap_rep, cur.full_cap_nom, cur.cycles);

	return 0;
}

void app_fuel_gauge_notify_charger_state(bool charging)
{
	static bool was_charging;
	struct max17055_data_snapshot snap;

	if (was_charging && !charging) {
		if (max17055_read_all(fg_dev, &snap) == 0 &&
		    snap.voltage_uv >= (int32_t)APP_CHARGE_COMPLETE_VOLTAGE_MV * 1000) {
			if (max17055_force_full(fg_dev) == 0) {
				charge_full_latched = true;
				printk("fuel_gauge: charge complete at %dmV - forced RepSOC to "
				       "100%%\n",
				       (int)(snap.voltage_uv / 1000));
			} else {
				printk("fuel_gauge: charge complete but force-full write "
				       "failed\n");
			}
		}
	}

	was_charging = charging;
}
