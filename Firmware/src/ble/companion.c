/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "charger.h"
#include "companion.h"
#include "companion_library.h"
#include "companion_proto.h"
#include "config.h"
#include "fuel_gauge.h"
#include "power.h"
#include "rtc.h"

#define APP_COMPANION_FW_VERSION "0.1"

/* The UUID _VAL macros expand to five arguments; this forces that expansion
 * before BT_UUID_128_ENCODE() counts them.
 */
#define UUID128_ENCODE(...) BT_UUID_128_ENCODE(__VA_ARGS__)

#define NAME_SUFFIX_LEN 4
#define NAME_BUF_LEN (sizeof(APP_COMPANION_NAME_PREFIX) + NAME_SUFFIX_LEN)

#define DISCONNECT_WAIT_MS 2000

/* Serialises start/stop/timeout/advertising restart. Never taken from a BT
 * callback, and never held by anything a BT thread waits on, because
 * bt_disable() blocks on those threads.
 */
static K_MUTEX_DEFINE(ctrl_lock);

/* Guards everything below; held only for field updates. */
static struct k_spinlock lock;
static bool active;
static enum app_companion_state state = APP_COMPANION_STATE_OFF;
static uint32_t cur_passkey;
static app_companion_state_cb_t state_cb;
static void (*link_cb)(void);
static bool notify_pending;
static struct bt_conn *cur_conn;

static bool callbacks_registered;

/* bt_enable() leaves its "enabled" flag set when it fails part-way (e.g. the
 * settings backend won't mount), so a retry returns -EALREADY and carries on
 * against a controller that was never opened, which hangs. Remember the
 * failure instead; only a reboot clears it.
 */
static int enable_failure;
static char dev_name[NAME_BUF_LEN];
static struct bt_data scan_rsp[1];

static K_SEM_DEFINE(disc_sem, 0, 1);

static void notify_handler(struct k_work *work);
static void timeout_handler(struct k_work *work);
static void adv_restart_handler(struct k_work *work);

static K_WORK_DEFINE(notify_work, notify_handler);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_handler);
static K_WORK_DEFINE(adv_restart_work, adv_restart_handler);

static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, UUID128_ENCODE(APP_COMPANION_UUID_SERVICE_VAL)),
};

static const struct bt_le_adv_param adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);

/* ------------------------------------------------------------------ */
/* State delivery                                                     */
/* ------------------------------------------------------------------ */

static void publish(enum app_companion_state new_state, uint32_t passkey)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	state = new_state;
	cur_passkey = passkey;
	if (state_cb || link_cb) {
		notify_pending = true;
		k_work_submit(&notify_work);
	}

	k_spin_unlock(&lock, key);
}

/* Same, but dropped once the session is being torn down so a late BT callback
 * cannot overwrite the final state.
 */
static void publish_active(enum app_companion_state new_state, uint32_t passkey)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool is_active = active;

	k_spin_unlock(&lock, key);

	if (is_active) {
		publish(new_state, passkey);
	}
}

static void notify_handler(struct k_work *work)
{
	app_companion_state_cb_t cb;
	void (*link)(void);
	enum app_companion_state s;
	uint32_t pk;
	bool pending;
	k_spinlock_key_t key;

	ARG_UNUSED(work);

	key = k_spin_lock(&lock);
	cb = state_cb;
	link = link_cb;
	s = state;
	pk = cur_passkey;
	pending = notify_pending;
	notify_pending = false;
	k_spin_unlock(&lock, key);

	if (!pending) {
		return;
	}

	if (cb) {
		cb(s, pk);
	}

	if (link) {
		link();
	}
}

void app_companion_set_link_callback(void (*cb)(void))
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	link_cb = cb;
	k_spin_unlock(&lock, key);
}

/* ------------------------------------------------------------------ */
/* Connection and pairing callbacks (BT threads)                      */
/* ------------------------------------------------------------------ */

static void restart_timeout_window(void)
{
	k_work_reschedule(&timeout_work, K_SECONDS(APP_COMPANION_ADV_TIMEOUT_SEC));
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	k_spinlock_key_t key;
	bool accept;

	if (err) {
		printk("companion: connection failed (0x%02x)\n", err);
		return;
	}

	key = k_spin_lock(&lock);
	accept = active && cur_conn == NULL;
	if (accept) {
		cur_conn = bt_conn_ref(conn);
	}
	k_spin_unlock(&lock, key);

	if (!accept) {
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	printk("companion: phone connected, waiting for pairing\n");

	/* The advertising window now covers pairing; it is cancelled once the
	 * link is encrypted and bonded.
	 */
	restart_timeout_window();

	/* Starts pairing straight away on a fresh phone instead of waiting for
	 * its first (rejected) read, and just encrypts on a bonded one.
	 */
	err = bt_conn_set_security(conn, BT_SECURITY_L4);
	if (err) {
		printk("companion: set_security failed (%d)\n", err);
	}
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	k_spinlock_key_t key;
	bool ours;
	bool is_active;

	key = k_spin_lock(&lock);
	ours = (cur_conn == conn);
	if (ours) {
		bt_conn_unref(cur_conn);
		cur_conn = NULL;
	}
	is_active = active;
	k_spin_unlock(&lock, key);

	if (!ours) {
		return;
	}

	printk("companion: phone disconnected (0x%02x)\n", reason);
	k_sem_give(&disc_sem);

	if (is_active) {
		publish_active(APP_COMPANION_STATE_ADVERTISING, 0);
		restart_timeout_window();
	}
}

/* Connectable advertising holds a preallocated connection object, so with
 * BT_MAX_CONN=1 it can only restart once the old one is recycled.
 */
static void recycled(void)
{
	k_work_submit(&adv_restart_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		printk("companion: security failed (%d)\n", err);
		bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
		return;
	}

	printk("companion: security level %d\n", level);

	if (level >= BT_SECURITY_L4) {
		k_work_cancel_delayable(&timeout_work);
		publish_active(APP_COMPANION_STATE_CONNECTED, 0);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.security_changed = security_changed,
	.recycled = recycled,
};

static void auth_passkey_display(struct bt_conn *conn, unsigned int passkey)
{
	ARG_UNUSED(conn);

	printk("companion: passkey %06u\n", passkey);
	publish_active(APP_COMPANION_STATE_PASSKEY, passkey);
}

static void auth_cancel(struct bt_conn *conn)
{
	printk("companion: pairing cancelled\n");
	bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static const struct bt_conn_auth_cb auth_cb = {
	.passkey_display = auth_passkey_display,
	.cancel = auth_cancel,
};

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
	ARG_UNUSED(conn);

	printk("companion: pairing complete (bonded=%d)\n", bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
	printk("companion: pairing failed (%d)\n", reason);
	bt_conn_disconnect(conn, BT_HCI_ERR_AUTH_FAIL);
}

static void bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	ARG_UNUSED(id);
	ARG_UNUSED(peer);

	printk("companion: bond deleted\n");
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = pairing_complete,
	.pairing_failed = pairing_failed,
	.bond_deleted = bond_deleted,
};

/* ------------------------------------------------------------------ */
/* GATT service                                                       */
/* ------------------------------------------------------------------ */

static ssize_t read_device_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	struct app_companion_device_info info = {
		.proto_version = APP_COMPANION_PROTO_VERSION,
		.charging = app_charger_is_charging() ? 1 : 0,
		.sd_free_kb = APP_COMPANION_SD_SIZE_UNKNOWN,
		.sd_total_kb = APP_COMPANION_SD_SIZE_UNKNOWN,
	};

	strncpy(info.fw_version, APP_COMPANION_FW_VERSION, sizeof(info.fw_version) - 1);

	if (app_fuel_gauge_get_percent(&info.battery_pct)) {
		info.battery_pct = 0xFF;
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, &info, sizeof(info));
}

static bool is_leap_year(unsigned int y)
{
	return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

static uint8_t days_in_month(unsigned int year, unsigned int month)
{
	static const uint8_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

	return (month == 2 && is_leap_year(year)) ? 29 : days[month - 1];
}

static ssize_t write_time(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			  uint16_t len, uint16_t offset, uint8_t flags)
{
	struct app_companion_time t;
	struct rtc_time rt = {0};
	int ret;

	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != sizeof(t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(&t, buf, sizeof(t));

	/* The PCF85063A only holds 2000..2099. */
	if (t.year < 2000 || t.year > 2099 || t.month < 1 || t.month > 12 || t.day < 1 ||
	    t.day > days_in_month(t.year, t.month) || t.hour > 23 || t.min > 59 || t.sec > 59) {
		printk("companion: TIME rejected (%u-%u-%u %u:%u:%u)\n", t.year, t.month, t.day,
		       t.hour, t.min, t.sec);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	rt.tm_year = t.year - 1900;
	rt.tm_mon = t.month - 1;
	rt.tm_mday = t.day;
	rt.tm_hour = t.hour;
	rt.tm_min = t.min;
	rt.tm_sec = t.sec;

	ret = app_rtc_set_time(&rt);
	if (ret) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	printk("companion: time set to %04u-%02u-%02u %02u:%02u:%02u\n", t.year, t.month, t.day,
	       t.hour, t.min, t.sec);
	return len;
}

static int lib_send(struct bt_conn *conn, const uint8_t *data, uint16_t len);

static ssize_t write_lib_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	int err;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	/* Runs on the BT RX thread: the library service only queues the command. */
	err = app_companion_lib_submit(conn, buf, len, lib_send);
	if (err == -EINVAL) {
		printk("companion: LIB_CTRL rejected (len %u)\n", len);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	if (err == -EBUSY) {
		/* The running request will end with its own END frame. */
		printk("companion: LIB_CTRL busy\n");
		return BT_GATT_ERR(BT_ATT_ERR_PREPARE_QUEUE_FULL);
	}
	if (err) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}

	return len;
}

static ssize_t write_cal_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	printk("companion: CAL_CTRL not implemented yet\n");
	return len;
}

static ssize_t write_cal_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	printk("companion: CAL_DATA not implemented yet\n");
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);

	printk("companion: notifications %s\n", value == BT_GATT_CCC_NOTIFY ? "on" : "off");
}

#define COMPANION_UUID(val) BT_UUID_INIT_128(UUID128_ENCODE(val))

static const struct bt_uuid_128 uuid_service = COMPANION_UUID(APP_COMPANION_UUID_SERVICE_VAL);
static const struct bt_uuid_128 uuid_device_info = COMPANION_UUID(APP_COMPANION_UUID_DEVICE_INFO_VAL);
static const struct bt_uuid_128 uuid_time = COMPANION_UUID(APP_COMPANION_UUID_TIME_VAL);
static const struct bt_uuid_128 uuid_lib_ctrl = COMPANION_UUID(APP_COMPANION_UUID_LIB_CTRL_VAL);
static const struct bt_uuid_128 uuid_lib_data = COMPANION_UUID(APP_COMPANION_UUID_LIB_DATA_VAL);
static const struct bt_uuid_128 uuid_cal_ctrl = COMPANION_UUID(APP_COMPANION_UUID_CAL_CTRL_VAL);
static const struct bt_uuid_128 uuid_cal_data = COMPANION_UUID(APP_COMPANION_UUID_CAL_DATA_VAL);
static const struct bt_uuid_128 uuid_cal_status = COMPANION_UUID(APP_COMPANION_UUID_CAL_STATUS_VAL);

#define CCC_PERM (BT_GATT_PERM_READ_AUTHEN | BT_GATT_PERM_WRITE_AUTHEN)

BT_GATT_SERVICE_DEFINE(companion_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_service),

	BT_GATT_CHARACTERISTIC(&uuid_device_info.uuid, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ_AUTHEN, read_device_info, NULL, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_time.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, write_time, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_lib_ctrl.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, write_lib_ctrl, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_lib_data.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, CCC_PERM),

	BT_GATT_CHARACTERISTIC(&uuid_cal_ctrl.uuid, BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, write_cal_ctrl, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_cal_data.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_AUTHEN, NULL, write_cal_data, NULL),

	BT_GATT_CHARACTERISTIC(&uuid_cal_status.uuid, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed, CCC_PERM),
);

/* Called from the library worker thread; blocks for a TX buffer when the
 * controller queue is full.
 */
static int lib_send(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
	const struct bt_gatt_attr *attr =
		bt_gatt_find_by_uuid(companion_svc.attrs, companion_svc.attr_count, &uuid_lib_data.uuid);

	if (attr == NULL) {
		return -ENOENT;
	}

	if (!bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) {
		return -EACCES;
	}

	return bt_gatt_notify_uuid(conn, &uuid_lib_data.uuid, companion_svc.attrs, data, len);
}

/* ------------------------------------------------------------------ */
/* Session control                                                    */
/* ------------------------------------------------------------------ */

static void build_name(void)
{
	uint8_t id[8];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));
	unsigned int tail = 0;

	if (n >= 2) {
		tail = (id[n - 2] << 8) | id[n - 1];
	}

	snprintf(dev_name, sizeof(dev_name), APP_COMPANION_NAME_PREFIX "%04X", tail);
}

static int start_advertising(void)
{
	int err = bt_le_adv_start(&adv_param, adv_data, ARRAY_SIZE(adv_data), scan_rsp,
				  ARRAY_SIZE(scan_rsp));

	return (err == -EALREADY) ? 0 : err;
}

static void adv_restart_handler(struct k_work *work)
{
	int err = 0;

	ARG_UNUSED(work);

	k_mutex_lock(&ctrl_lock, K_FOREVER);

	k_spinlock_key_t key = k_spin_lock(&lock);
	bool can_advertise = active && cur_conn == NULL;

	k_spin_unlock(&lock, key);

	if (can_advertise) {
		err = start_advertising();
		if (err) {
			printk("companion: advertising restart failed (%d)\n", err);
		}
	}

	k_mutex_unlock(&ctrl_lock);
}

/* Takes the session down. Caller holds no locks. */
static void teardown(enum app_companion_state final_state, bool report)
{
	struct bt_conn *conn = NULL;
	k_spinlock_key_t key;
	int err;

	k_mutex_lock(&ctrl_lock, K_FOREVER);

	key = k_spin_lock(&lock);
	if (!active) {
		/* Already torn down (e.g. timed out earlier); stop() on a
		 * TIMED_OUT session just clears the sticky state.
		 */
		if (!report) {
			state = APP_COMPANION_STATE_OFF;
			state_cb = NULL;
			notify_pending = false;
		}
		k_spin_unlock(&lock, key);
		k_mutex_unlock(&ctrl_lock);
		return;
	}
	active = false;
	if (!report) {
		state_cb = NULL;
		notify_pending = false;
	}
	if (cur_conn) {
		conn = bt_conn_ref(cur_conn);
	}
	k_spin_unlock(&lock, key);

	k_work_cancel_delayable(&timeout_work);
	k_work_cancel(&adv_restart_work);

	err = bt_le_adv_stop();
	if (err && err != -EALREADY) {
		printk("companion: adv stop failed (%d)\n", err);
	}

	if (conn) {
		k_sem_reset(&disc_sem);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		bt_conn_unref(conn);
		if (k_sem_take(&disc_sem, K_MSEC(DISCONNECT_WAIT_MS)) != 0) {
			printk("companion: disconnect wait timed out\n");
		}
	}

	err = bt_disable();
	if (err) {
		printk("companion: bt_disable failed (%d)\n", err);
	}

	app_power_set_sleep_inhibited(false);
	app_power_mark_activity();

	if (report) {
		publish(final_state, 0);
	} else {
		key = k_spin_lock(&lock);
		state = APP_COMPANION_STATE_OFF;
		k_spin_unlock(&lock, key);
	}

	printk("companion: stopped\n");

	k_mutex_unlock(&ctrl_lock);
}

static void timeout_handler(struct k_work *work)
{
	k_spinlock_key_t key;
	bool secured;

	ARG_UNUSED(work);

	key = k_spin_lock(&lock);
	secured = (state == APP_COMPANION_STATE_CONNECTED);
	k_spin_unlock(&lock, key);

	if (secured) {
		return;
	}

	printk("companion: advertising window expired\n");
	teardown(APP_COMPANION_STATE_TIMED_OUT, true);
}

int app_companion_start(app_companion_state_cb_t cb)
{
	k_spinlock_key_t key;
	int err;

	k_mutex_lock(&ctrl_lock, K_FOREVER);

	if (active) {
		k_mutex_unlock(&ctrl_lock);
		return -EALREADY;
	}

	if (enable_failure) {
		err = enable_failure;
		goto out;
	}

	err = bt_enable(NULL);
	if (err && err != -EALREADY) {
		printk("companion: bt_enable failed (%d)\n", err);
		enable_failure = err;
		goto out;
	}

	/* bt_disable() drops the identity and keys, and the stack finishes
	 * initialising only once settings are loaded, so this is needed on every
	 * enable, not just the first.
	 */
	err = settings_load();
	if (err) {
		printk("companion: settings_load failed (%d)\n", err);
		goto fail_disable;
	}

	if (!callbacks_registered) {
		err = bt_conn_auth_cb_register(&auth_cb);
		if (err) {
			printk("companion: auth cb register failed (%d)\n", err);
			goto fail_disable;
		}

		err = bt_conn_auth_info_cb_register(&auth_info_cb);
		if (err) {
			printk("companion: auth info cb register failed (%d)\n", err);
			bt_conn_auth_cb_register(NULL);
			goto fail_disable;
		}

		callbacks_registered = true;
	}

	build_name();
	err = bt_set_name(dev_name);
	if (err) {
		printk("companion: bt_set_name failed (%d)\n", err);
	}
	scan_rsp[0] = (struct bt_data)BT_DATA(BT_DATA_NAME_COMPLETE, dev_name, strlen(dev_name));

	bt_set_bondable(true);

	key = k_spin_lock(&lock);
	active = true;
	state = APP_COMPANION_STATE_ADVERTISING;
	cur_passkey = 0;
	state_cb = cb;
	notify_pending = false;
	k_spin_unlock(&lock, key);

	err = start_advertising();
	if (err) {
		printk("companion: advertising start failed (%d)\n", err);
		key = k_spin_lock(&lock);
		active = false;
		state = APP_COMPANION_STATE_OFF;
		state_cb = NULL;
		k_spin_unlock(&lock, key);
		goto fail_disable;
	}

	app_power_set_sleep_inhibited(true);
	restart_timeout_window();

	printk("companion: advertising as \"%s\"\n", dev_name);
	publish(APP_COMPANION_STATE_ADVERTISING, 0);

	k_mutex_unlock(&ctrl_lock);
	return 0;

fail_disable:
	if (bt_disable()) {
		printk("companion: bt_disable failed during cleanup\n");
	}
out:
	k_mutex_unlock(&ctrl_lock);
	return err;
}

void app_companion_stop(void)
{
	teardown(APP_COMPANION_STATE_OFF, false);
}

enum app_companion_state app_companion_get_state(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	enum app_companion_state s = state;

	k_spin_unlock(&lock, key);
	return s;
}
