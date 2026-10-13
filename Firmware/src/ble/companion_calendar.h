/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Receiving side of the calendar push (CAL_CTRL / CAL_DATA / CAL_STATUS in
 * companion_proto.h). The blob is collected in RAM; nothing touches the SD card
 * until COMMIT, so an interrupted transfer leaves the stored calendar alone.
 */

#ifndef APP_COMPANION_CALENDAR_H_
#define APP_COMPANION_CALENDAR_H_

#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

/* Pushes bytes out as one notification on CAL_STATUS. Same contract as
 * app_companion_lib_send_t: 0 or a negative errno (not subscribed, ...). A
 * failed send is only logged, the phone times out on its own.
 */
typedef int (*app_companion_cal_send_t)(struct bt_conn *conn, const uint8_t *data, uint16_t len);

/* Handles a CAL_CTRL write (BEGIN / COMMIT / ABORT). Does no I/O and sends
 * nothing itself, so it is safe from the BT RX thread: the CAL_STATUS answer
 * for BEGIN and COMMIT is sent later from the library work queue thread.
 * Returns 0, -EINVAL for a malformed command (wrong length, unknown op),
 * -EBUSY if a previous BEGIN/COMMIT has not finished answering yet, or
 * -ENOTCONN if the link is already gone.
 */
int app_companion_cal_ctrl(struct bt_conn *conn, const void *cmd, uint16_t len,
			   app_companion_cal_send_t send);

/* Handles a CAL_DATA write (write without response, so no error can be
 * returned to the phone): appends to the blob. Data outside a BEGIN..COMMIT
 * window, or past the announced total_len, is dropped; overflow makes COMMIT
 * fail with BAD_REQUEST.
 */
void app_companion_cal_data(const void *data, uint16_t len);

/* Drops a partially received blob (link lost, companion mode ended). A COMMIT
 * already running is left to finish.
 */
void app_companion_cal_reset(void);

#endif /* APP_COMPANION_CALENDAR_H_ */
