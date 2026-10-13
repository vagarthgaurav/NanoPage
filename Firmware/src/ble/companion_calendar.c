/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#include "calendar_store.h"
#include "companion_calendar.h"
#include "companion_library.h"
#include "companion_proto.h"
#include "power.h"
#include "usb_msc.h"

enum cal_state {
	CAL_IDLE,
	CAL_RECEIVING,
	CAL_COMMITTING,
};

/* The BT RX thread (CAL_CTRL / CAL_DATA) and the library work thread both touch
 * this. Held only for field updates and the CAL_DATA memcpy, never across I/O,
 * a notification or any other blocking call.
 */
static struct k_spinlock lock;
static enum cal_state state = CAL_IDLE;
static uint8_t blob[APP_CALENDAR_STORE_MAX_BLOB];
static uint32_t received;
static uint32_t total_len;
static uint32_t total_crc;
static uint16_t event_count;
static bool overflow;

/* Set while the worker owns `pending`: from a BEGIN/COMMIT being accepted until
 * its answer is sent. COMMITTING implies busy, so the blob is never touched by
 * a second BEGIN while the save runs.
 */
static atomic_t busy;

static struct {
	struct bt_conn *conn;
	app_companion_cal_send_t send;
	uint8_t op;
	enum app_companion_status status; /* answer, unless `commit` */
	bool commit;                      /* worker has to verify and save the blob first */
} pending;

static void cal_work_handler(struct k_work *work);

static K_WORK_DEFINE(cal_work, cal_work_handler);

/* Runs on the library work queue, so the SD access in save() is serialised with
 * LIST / DELETE. Returns the status to report; the blob is released (state IDLE)
 * before returning, whatever the outcome.
 */
static enum app_companion_status do_commit(void)
{
	enum app_companion_status status;
	k_spinlock_key_t key;
	uint32_t len;
	uint32_t crc;
	uint16_t count;
	int ret;

	/* Only this thread touches the blob and the announced values while the
	 * state is COMMITTING; the lock just orders the reads after BEGIN's writes.
	 */
	key = k_spin_lock(&lock);
	len = total_len;
	crc = total_crc;
	count = event_count;
	k_spin_unlock(&lock, key);

	app_power_mark_activity();

	if (crc32_ieee(blob, len) != crc) {
		printk("calendar: CRC mismatch\n");
		status = APP_COMPANION_STATUS_CRC_MISMATCH;
	} else if (app_calendar_store_validate(blob, len, count) != 0) {
		printk("calendar: blob failed validation\n");
		status = APP_COMPANION_STATUS_BAD_REQUEST;
	} else if (app_usb_msc_is_active()) {
		printk("calendar: storage busy (USB mass storage)\n");
		status = APP_COMPANION_STATUS_STORAGE_BUSY;
	} else {
		ret = app_calendar_store_save(blob, len);
		if (ret == 0) {
			printk("calendar: stored %u events, %u bytes\n", (unsigned int)count,
			       (unsigned int)len);
			status = APP_COMPANION_STATUS_OK;
		} else if (ret == -EBUSY) {
			printk("calendar: storage busy\n");
			status = APP_COMPANION_STATUS_STORAGE_BUSY;
		} else {
			printk("calendar: save failed (%d)\n", ret);
			status = APP_COMPANION_STATUS_INTERNAL;
		}
	}

	/* The buffer was in use until save() returned. */
	key = k_spin_lock(&lock);
	state = CAL_IDLE;
	k_spin_unlock(&lock, key);

	return status;
}

static void cal_work_handler(struct k_work *work)
{
	struct app_companion_cal_status st = {.op = pending.op, .status = pending.status};
	int ret;

	ARG_UNUSED(work);

	if (pending.commit) {
		st.status = do_commit();
	}

	ret = pending.send(pending.conn, (const uint8_t *)&st, sizeof(st));
	if (ret) {
		/* The phone times out on its own. */
		printk("calendar: status send failed (%d)\n", ret);
	}

	/* conn must not be touched after this; busy is released last so the
	 * next BEGIN/COMMIT cannot overwrite `pending` early.
	 */
	bt_conn_unref(pending.conn);
	pending.conn = NULL;
	atomic_clear(&busy);
}

int app_companion_cal_ctrl(struct bt_conn *conn, const void *cmd, uint16_t len,
			   app_companion_cal_send_t send)
{
	const uint8_t *bytes = cmd;
	struct app_companion_cal_begin begin = {0};
	enum app_companion_status status = APP_COMPANION_STATUS_OK;
	bool commit = false;
	k_spinlock_key_t key;
	struct bt_conn *ref;
	uint8_t op;

	if (len == 0) {
		return -EINVAL;
	}

	op = bytes[0];
	switch (op) {
	case APP_COMPANION_CAL_BEGIN:
		if (len != sizeof(begin)) {
			return -EINVAL;
		}
		memcpy(&begin, cmd, sizeof(begin));
		break;
	case APP_COMPANION_CAL_COMMIT:
	case APP_COMPANION_CAL_ABORT:
		if (len != 1) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (op == APP_COMPANION_CAL_ABORT) {
		/* Silent; a COMMIT already running finishes and answers on its own. */
		key = k_spin_lock(&lock);
		if (state == CAL_RECEIVING) {
			state = CAL_IDLE;
			printk("calendar: transfer aborted\n");
		}
		k_spin_unlock(&lock, key);
		return 0;
	}

	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}

	ref = bt_conn_ref(conn);
	if (ref == NULL) {
		atomic_clear(&busy);
		return -ENOTCONN;
	}

	key = k_spin_lock(&lock);
	if (state == CAL_COMMITTING) {
		/* Not reachable (COMMITTING implies busy), but never touch the blob then. */
		k_spin_unlock(&lock, key);
		bt_conn_unref(ref);
		atomic_clear(&busy);
		return -EBUSY;
	}

	if (op == APP_COMPANION_CAL_BEGIN) {
		/* A BEGIN while RECEIVING just restarts the transfer. */
		if (begin.total_len < sizeof(struct app_companion_cal_header)) {
			status = APP_COMPANION_STATUS_BAD_REQUEST;
		} else if (begin.total_len > APP_CALENDAR_STORE_MAX_BLOB ||
			   begin.event_count > APP_CALENDAR_STORE_MAX_EVENTS) {
			status = APP_COMPANION_STATUS_TOO_LARGE;
		}

		if (status == APP_COMPANION_STATUS_OK) {
			state = CAL_RECEIVING;
			received = 0;
			overflow = false;
			total_len = begin.total_len;
			total_crc = begin.crc32;
			event_count = begin.event_count;
		} else {
			state = CAL_IDLE;
		}
	} else if (state != CAL_RECEIVING || received != total_len || overflow) {
		status = APP_COMPANION_STATUS_BAD_REQUEST;
		state = CAL_IDLE;
	} else {
		state = CAL_COMMITTING;
		commit = true;
	}
	k_spin_unlock(&lock, key);

	if (status != APP_COMPANION_STATUS_OK) {
		printk("calendar: %s rejected (status %d)\n",
		       op == APP_COMPANION_CAL_BEGIN ? "BEGIN" : "COMMIT", status);
	} else if (op == APP_COMPANION_CAL_BEGIN) {
		printk("calendar: BEGIN %u bytes, %u events\n", (unsigned int)begin.total_len,
		       (unsigned int)begin.event_count);
	}

	pending.conn = ref;
	pending.send = send;
	pending.op = op;
	pending.status = status;
	pending.commit = commit;

	app_companion_lib_queue_work(&cal_work);

	return 0;
}

void app_companion_cal_data(const void *data, uint16_t len)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (state == CAL_RECEIVING) {
		if (len <= total_len - received) {
			/* Copied under the lock: at most one ATT payload (a few hundred
			 * bytes, microseconds), and it keeps reset / ABORT / BEGIN from
			 * moving `received` or flipping the state mid-copy.
			 */
			memcpy(&blob[received], data, len);
			received += len;
		} else {
			overflow = true;
		}
	}

	k_spin_unlock(&lock, key);
}

void app_companion_cal_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (state == CAL_RECEIVING) {
		state = CAL_IDLE;
	}

	k_spin_unlock(&lock, key);
}
