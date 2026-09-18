/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#include <stdbool.h>

/*
 * Records the boot-time reset cause (RTT debug aid) and starts the idle
 * clock. Call once from main(), after the other subsystem inits.
 */
void app_power_init(void);

/*
 * Call from any user-input event handler (button press, later a touch/USB
 * event if one's ever added) to reset the idle clock.
 */
void app_power_mark_activity(void);

/*
 * Call periodically from the main loop. If more than the idle timeout has
 * elapsed since the last app_power_mark_activity(), this puts the board
 * into nRF52840 System OFF - the lowest power state the SoC has - and does
 * not return. The nRF52840 has no RAM retention in System OFF, so the only
 * way out is a full reboot: either the physical reset button, or a press
 * of one of the three UI buttons (armed here as a GPIO SENSE wake source
 * right before power-off).
 *
 * No-ops for as long as app_power_set_sleep_inhibited(true) is in effect.
 */
void app_power_check_idle(void);

/*
 * Blocks (true) or allows (false) app_power_check_idle() from entering
 * System OFF, regardless of how long the board has been idle - for a
 * caller (usb_msc.c) that needs the SD card/display to stay powered for
 * as long as a USB host might be mid-transfer, since System OFF cuts both
 * (see enter_system_off() in power.c). Doesn't reset the idle clock
 * itself - the caller is expected to call app_power_mark_activity() once
 * it's done, so the full idle timeout starts fresh from that point rather
 * than picking up wherever it left off before the inhibit began.
 */
void app_power_set_sleep_inhibited(bool inhibited);

#endif
