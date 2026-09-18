/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_I2C1_BUS_H_
#define APP_I2C1_BUS_H_

/*
 * i2c1 (TWIM1) carries both the RTC (rtc.c) and the fuel gauge
 * (max17055.c) - see the TODO item this closes out in TODO.md and the
 * "PPK2" investigation in .claude/CLAUDE.md for the spi3 precedent this
 * mirrors. CONFIG_PM_DEVICE_RUNTIME's automatic get/put refcounting was
 * dropped project-wide (spi3's own idle-current bug, fixed 2026-09-17)
 * in favor of explicit pm_device_action_run() calls - spi3 got that
 * treatment then (display.c's app_display_lock()/unlock()); i2c1 never
 * did, since it was never implicated in that investigation (the
 * display-driver-alone bisection reproduced the whole idle-current gap
 * without RTC/fuel-gauge involvement) - this closes that gap for
 * completeness/correctness, not because a matching leak was ever
 * measured on i2c1.
 *
 * app_i2c1_lock()/app_i2c1_unlock() bracket every actual i2c1
 * transaction, refcounted so nested calls (e.g. rtc.c's alarm
 * trampoline invoking a callback that itself reads the fuel gauge)
 * compose correctly instead of the inner call's unlock suspending the
 * bus out from under the outer one - same pattern as
 * display.c's spi3_pm_refcount. Callers are rtc.c and max17055.c, one
 * lock/unlock pair per exported function (same "whole operation, not
 * every individual register access" granularity display.c uses for
 * spi3) - not meant to be called from anywhere else.
 */
void app_i2c1_lock(void);
void app_i2c1_unlock(void);

#endif /* APP_I2C1_BUS_H_ */
