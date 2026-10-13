/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_COMPANION_LIBRARY_H_
#define APP_COMPANION_LIBRARY_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

/* Pushes bytes out as one notification on LIB_DATA. Returns 0 or a negative
 * errno (not connected, not subscribed, ...); any error aborts the request.
 */
typedef int (*app_companion_lib_send_t)(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/* Handles a LIB_CTRL write. Copies the command, takes a conn reference and
 * queues the work on a dedicated thread, so it does no I/O and is safe from the
 * BT RX thread. Returns 0, -EINVAL for a malformed command, -EBUSY if a request
 * is already running (the caller answers nothing; the phone has to wait for the
 * END frame of the running one), or -ENOTCONN if the link is already gone.
 */
int app_companion_lib_submit(struct bt_conn *conn, const void *cmd, uint16_t len,
			     app_companion_lib_send_t send);

/* SD usage in KiB as of the last library request that mounted the card (LIST,
 * CHAPTERS, DELETE). Returns false if none has completed yet. Never touches the
 * card, so it is safe from the BT RX thread.
 */
bool app_companion_lib_get_sd_usage(uint32_t *free_kb, uint32_t *total_kb);

#endif /* APP_COMPANION_LIBRARY_H_ */
