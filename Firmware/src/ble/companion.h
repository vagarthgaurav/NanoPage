/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_COMPANION_H_
#define APP_COMPANION_H_

#include <stdint.h>

enum app_companion_state {
	APP_COMPANION_STATE_OFF,         /* radio off, stack disabled */
	APP_COMPANION_STATE_ADVERTISING, /* waiting for a phone, times out */
	APP_COMPANION_STATE_PASSKEY,     /* pairing; passkey is valid */
	APP_COMPANION_STATE_CONNECTED,   /* encrypted + bonded link up */
	APP_COMPANION_STATE_TIMED_OUT,   /* advertising window expired, back to OFF */
};

/* Always called from the system workqueue (never from a BT thread), so the
 * callback may draw and refresh the display. `passkey` is only meaningful in
 * APP_COMPANION_STATE_PASSKEY (6 digits, 0..999999).
 */
typedef void (*app_companion_state_cb_t)(enum app_companion_state state, uint32_t passkey);

/* Enter companion mode: bt_enable() if needed, start advertising for
 * APP_COMPANION_ADV_TIMEOUT_SEC (config.h). Returns 0 or negative errno.
 * Only one session at a time; -EALREADY if already active.
 */
int app_companion_start(app_companion_state_cb_t cb);

/* Leave companion mode: disconnect, stop advertising, bt_disable(). Safe to
 * call when already off. No callback is delivered for a stop the caller
 * requested itself.
 */
void app_companion_stop(void);

enum app_companion_state app_companion_get_state(void);

/* Called from the system workqueue after every state change, whether or not a
 * screen is attached, so the header can show the link. Pass NULL to clear.
 */
void app_companion_set_link_callback(void (*cb)(void));

#endif /* APP_COMPANION_H_ */
