/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

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
 */
void app_power_check_idle(void);

/*
 * If the boot cause recorded by app_power_init() was a wake from System
 * OFF, briefly shows a "Waking up" message on the panel before the caller
 * draws its real content. No-op on a normal power-on or debugger reset.
 * Call after app_text_init() has succeeded.
 */
void app_power_show_wake_message(void);

#endif
