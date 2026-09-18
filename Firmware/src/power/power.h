/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_POWER_H_
#define APP_POWER_H_

#include <stdbool.h>

/*
 * If the idle timer below ever fires with no app_power_mark_activity() call
 * in the preceding APP_POWER_IDLE_TIMEOUT_MS, the board drops straight into
 * nRF52840 System OFF - the lowest power state the SoC has. System OFF has
 * no RAM retention, so the only way out is a full reboot: either the
 * physical reset button, or a press of one of the three UI buttons (armed
 * as a GPIO SENSE wake source right before power-off).
 */

/*
 * Records the boot-time reset cause (RTT debug aid) and arms the idle timer
 * for the first time (APP_POWER_IDLE_TIMEOUT_MS out from this call). Call
 * once from main(), after the other subsystem inits.
 */
void app_power_init(void);

/*
 * Whether this boot followed a real System OFF wake (a button press, per
 * the GPIO SENSE wake sources armed in enter_system_off()) rather than the
 * reset button or a fresh flash - see print_and_clear_reset_cause() in
 * power.c for how this is determined from RESETREAS. main.c uses this to
 * decide whether to try resuming whatever reader session was open when the
 * idle timer triggered that System OFF (app_reader_resume_last_session())
 * instead of booting to the app menu as normal - a plain reset/reflash
 * always boots to the menu, even if a stale resume marker is sitting on
 * the SD card from an earlier session.
 */
bool app_power_woke_from_system_off(void);

/*
 * Call from any user-input event handler (button press, USB MSC session
 * end) to push the idle deadline out another APP_POWER_IDLE_TIMEOUT_MS.
 * Implemented as a k_work_delayable reschedule rather than a timestamp
 * some other subsystem polls, so entering System OFF (see below) happens
 * at exactly the configured timeout, not whenever the next unrelated
 * periodic tick happens to notice.
 *
 * No-ops for as long as app_power_set_sleep_inhibited(true) is in effect -
 * see that function's doc comment for why.
 */
void app_power_mark_activity(void);

/*
 * Blocks (true) or allows (false) the idle timer from ever firing,
 * regardless of how long the board has been idle - for a caller
 * (usb_msc.c) that needs the SD card/display to stay powered for as long
 * as a USB host might be mid-transfer, since System OFF cuts both (see
 * enter_system_off() in power.c). Setting true cancels any pending idle
 * timer outright; it does not itself reschedule anything on the way back
 * to false - the caller is expected to call app_power_mark_activity()
 * once it's done, so the full idle timeout starts fresh from that point
 * rather than picking up wherever it left off before the inhibit began.
 */
void app_power_set_sleep_inhibited(bool inhibited);

/*
 * Registers a callback that enter_system_off() (power.c) invokes right
 * before it touches the SD card/display, so a screen with state that's
 * only persisted lazily gets one last chance to flush it before an
 * idle-triggered System OFF discards it along with the rest of RAM -
 * reader.c is the current user (app_reader_set_font() doesn't save the
 * new font choice until the next page turn/bookmark toggle; sitting idle
 * right after a font change would otherwise silently revert it on the
 * next boot). power.c is a low-level subsystem with no business including
 * a UI screen's header directly - this indirection is what keeps it that
 * way, same reasoning as reader.c/reader_menu.c threading `exit_cb`
 * function pointers between screens instead of calling each other
 * directly. NULL clears it; a single slot is enough since reader.c is the
 * only screen that currently needs this - revisit if a second one ever
 * does.
 */
void app_power_set_idle_hook(void (*cb)(void));

#endif
