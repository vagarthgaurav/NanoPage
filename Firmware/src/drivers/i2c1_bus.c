/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>

#include "i2c1_bus.h"

static const struct device *const i2c1_dev = DEVICE_DT_GET(DT_NODELABEL(i2c1));

static K_MUTEX_DEFINE(i2c1_bus_lock);
static int i2c1_pm_refcount;

void app_i2c1_lock(void)
{
	k_mutex_lock(&i2c1_bus_lock, K_FOREVER);

	if (i2c1_pm_refcount == 0) {
		pm_device_action_run(i2c1_dev, PM_DEVICE_ACTION_RESUME);
	}
	i2c1_pm_refcount++;
}

void app_i2c1_unlock(void)
{
	i2c1_pm_refcount--;
	if (i2c1_pm_refcount == 0) {
		pm_device_action_run(i2c1_dev, PM_DEVICE_ACTION_SUSPEND);
	}

	k_mutex_unlock(&i2c1_bus_lock);
}
