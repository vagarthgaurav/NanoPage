/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/fs/fs.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "companion_library.h"
#include "companion_proto.h"
#include "npb_bookmark_format.h"
#include "npb_format.h"
#include "power.h"
#include "sd.h"
#include "usb_msc.h"

#define LIB_STACK_SIZE 4096 /* FatFs call chains need ~4K, see prj.conf */
#define LIB_PRIORITY   K_PRIO_PREEMPT(10)

/* Directories scanned below the root. Root + this many levels are open at once,
 * which just fits CONFIG_FS_FATFS_NUM_DIRS (4).
 */
#define LIST_MAX_DEPTH 3

#define BOOK_EXT       ".npb"
#define BMK_DIR        ".nanopage" /* same sidecar dir as reader.c */
#define STAGE_LEN      (CONFIG_BT_L2CAP_TX_MTU - 3)
#define ATT_HDR_LEN    3
#define VPATH_LEN      (APP_COMPANION_BOOK_ID_LEN + 2) /* leading '/', id, NUL */

struct scan_level {
	struct fs_dir_t dir;
	struct fs_dirent ent;
};

/* Only one request runs at a time, so everything lives in statics: the stack
 * is small and FIL carries a 512 byte sector buffer.
 */
static K_THREAD_STACK_DEFINE(lib_stack, LIB_STACK_SIZE);
static struct k_work_q lib_workq;
static struct k_work lib_work;

static atomic_t busy;

static struct {
	struct bt_conn *conn;
	app_companion_lib_send_t send;
	struct app_companion_lib_cmd cmd;
	uint8_t stage[STAGE_LEN];
	uint16_t stage_len;
	int err; /* first send error; once set, nothing more is sent */
} req;

/* Written by the work thread, read from the BT RX thread (DEVICE_INFO). */
static struct k_spinlock usage_lock;
static uint32_t usage_free_kb = APP_COMPANION_SD_SIZE_UNKNOWN;
static uint32_t usage_total_kb = APP_COMPANION_SD_SIZE_UNKNOWN;

static char vpath[VPATH_LEN]; /* "/dir/book.npb": SD-relative with leading slash */
static char full_path[APP_SD_MAX_PATH_LEN];
static char bmk_rel[APP_SD_MAX_PATH_LEN];
static struct fs_file_t file;
static struct npb_header hdr;
static struct scan_level levels[LIST_MAX_DEPTH + 1];
static struct app_companion_book_record book_rec;
static struct npb_chapter chapter;
static struct app_companion_chapter_record chapter_rec;

/* ------------------------------------------------------------------ */
/* Frame output                                                       */
/* ------------------------------------------------------------------ */

static int stage_flush(void)
{
	uint16_t off = 0;

	while (off < req.stage_len && !req.err) {
		/* Re-read per notification, the phone can renegotiate. */
		uint16_t mtu = bt_gatt_get_mtu(req.conn);
		uint16_t room;
		uint16_t n;

		if (mtu <= ATT_HDR_LEN) {
			req.err = -ENOTCONN;
			break;
		}

		room = mtu - ATT_HDR_LEN;
		n = MIN(room, (uint16_t)(req.stage_len - off));

		/* Blocks for a TX buffer, which is the flow control. */
		req.err = req.send(req.conn, &req.stage[off], n);
		off += n;
	}

	req.stage_len = 0;
	return req.err;
}

static int stage_append(const void *data, uint16_t len)
{
	const uint8_t *p = data;

	while (len > 0 && !req.err) {
		uint16_t n = MIN(len, (uint16_t)(sizeof(req.stage) - req.stage_len));

		memcpy(&req.stage[req.stage_len], p, n);
		req.stage_len += n;
		p += n;
		len -= n;

		if (req.stage_len == sizeof(req.stage)) {
			stage_flush();
		}
	}

	return req.err;
}

static int emit_frame(uint8_t type, const void *payload, uint16_t len)
{
	struct app_companion_frame_hdr fh = {.type = type, .len = len};

	if (stage_append(&fh, sizeof(fh))) {
		return req.err;
	}

	return stage_append(payload, len);
}

static void emit_end(uint8_t op, enum app_companion_status status)
{
	struct app_companion_end_frame end = {.op = op, .status = status};

	if (emit_frame(APP_COMPANION_FRAME_END, &end, sizeof(end)) == 0) {
		stage_flush();
	}
}

/* ------------------------------------------------------------------ */
/* Book files                                                         */
/* ------------------------------------------------------------------ */

static bool has_book_ext(const char *name)
{
	size_t len = strlen(name);
	size_t ext = sizeof(BOOK_EXT) - 1;

	return len > ext && strcasecmp(name + len - ext, BOOK_EXT) == 0;
}

/* Opens `file` on vpath and validates the header. On 0 the file stays open and
 * `hdr` is filled; on error nothing is left open. -ENOENT: no such file,
 * -EINVAL: not a usable .npb, others: SD/FS errors.
 */
static int open_book(void)
{
	ssize_t n;
	int ret;

	ret = app_sd_full_path(vpath, full_path, sizeof(full_path));
	if (ret) {
		return ret;
	}

	fs_file_t_init(&file);
	ret = fs_open(&file, full_path, FS_O_READ);
	if (ret) {
		return ret;
	}

	n = fs_read(&file, &hdr, sizeof(hdr));
	if (n < 0) {
		fs_close(&file);
		return (int)n;
	}

	if (n != sizeof(hdr) || memcmp(hdr.magic, NPB_MAGIC, sizeof(hdr.magic)) != 0 ||
	    hdr.version != NPB_VERSION) {
		fs_close(&file);
		return -EINVAL;
	}

	return 0;
}

/* Puts the full path of "<dir>/.nanopage/<base>.bmk" for vpath into full_path. */
static int bookmark_full_path(void)
{
	const char *slash = strrchr(vpath, '/'); /* always present, vpath starts with '/' */
	size_t dir_len = slash - vpath;
	int ret;

	ret = snprintf(bmk_rel, sizeof(bmk_rel), "%.*s/" BMK_DIR "/%s.bmk", (int)dir_len, vpath,
		       slash + 1);
	if (ret < 0 || (size_t)ret >= sizeof(bmk_rel)) {
		return -ENAMETOOLONG;
	}

	return app_sd_full_path(bmk_rel, full_path, sizeof(full_path));
}

/* Reads last_position and last_read_local from the bookmark sidecar for
 * vpath; both 0 if the sidecar is missing or not ours. last_read_local is 0
 * for old sidecars that predate the field.
 */
static void read_sidecar(uint32_t *pos, uint32_t *last_read)
{
	uint8_t buf[offsetof(struct npb_bookmark_file, last_position) + sizeof(uint32_t)];
	ssize_t n;

	*pos = 0;
	*last_read = 0;

	if (bookmark_full_path()) {
		return;
	}

	fs_file_t_init(&file);
	if (fs_open(&file, full_path, FS_O_READ)) {
		return;
	}

	n = fs_read(&file, buf, sizeof(buf));
	if (n == sizeof(buf) &&
	    memcmp(buf, NPB_BOOKMARK_MAGIC, sizeof(((struct npb_bookmark_file *)0)->magic)) == 0) {
		memcpy(pos, &buf[offsetof(struct npb_bookmark_file, last_position)], sizeof(*pos));

		if (fs_seek(&file, offsetof(struct npb_bookmark_file, last_read_local),
			    FS_SEEK_SET) == 0 &&
		    fs_read(&file, last_read, sizeof(*last_read)) != sizeof(*last_read)) {
			*last_read = 0;
		}
	}

	fs_close(&file);
}

/* ------------------------------------------------------------------ */
/* LIST                                                               */
/* ------------------------------------------------------------------ */

static void emit_book(const struct fs_dirent *ent)
{
	struct app_companion_book_record *rec = &book_rec;
	uint32_t last_position, last_read;
	int ret = open_book();

	if (ret) {
		printk("library: skipping %s (%d)\n", vpath + 1, ret);
		return;
	}
	fs_close(&file);

	memset(rec, 0, sizeof(*rec));
	memcpy(rec->book_id, vpath + 1, strlen(vpath + 1));
	memcpy(rec->title, hdr.title, sizeof(rec->title));
	memcpy(rec->author, hdr.author, sizeof(rec->author));
	rec->file_size = ent->size;
	rec->text_blob_size = hdr.text_blob_size;
	rec->chapter_count = hdr.chapter_count;
	read_sidecar(&last_position, &last_read);
	rec->last_position = last_position;
	rec->last_read_local = last_read;

	emit_frame(APP_COMPANION_FRAME_BOOK, rec, sizeof(*rec));
}

/* vpath holds the directory to scan ("" for the root) and is restored before
 * returning. Returns 0 or the opendir error.
 */
static int scan_dir(unsigned int depth)
{
	struct scan_level *lv = &levels[depth];
	size_t base_len = strlen(vpath);
	int ret;

	ret = app_sd_full_path(base_len ? vpath : "/", full_path, sizeof(full_path));
	if (ret) {
		return ret;
	}

	fs_dir_t_init(&lv->dir);
	ret = fs_opendir(&lv->dir, full_path);
	if (ret) {
		printk("library: opendir %s failed (%d)\n", full_path, ret);
		return ret;
	}

	while (!req.err) {
		const char *name;
		size_t name_len;
		bool is_dir;

		ret = fs_readdir(&lv->dir, &lv->ent);
		if (ret || lv->ent.name[0] == '\0') {
			break;
		}

		name = lv->ent.name;
		if (name[0] == '.') {
			continue; /* "." / ".." and the .nanopage sidecar dir */
		}

		is_dir = (lv->ent.type == FS_DIR_ENTRY_DIR);
		if (is_dir ? depth >= LIST_MAX_DEPTH : !has_book_ext(name)) {
			continue;
		}

		/* The id is vpath minus the leading slash; a longer one could not
		 * be sent back in CHAPTERS/DELETE.
		 */
		name_len = strlen(name);
		if (base_len + 1 + name_len >= VPATH_LEN) {
			printk("library: skipping %s/%s, path too long\n", vpath, name);
			continue;
		}

		vpath[base_len] = '/';
		memcpy(&vpath[base_len + 1], name, name_len + 1);

		if (is_dir) {
			scan_dir(depth + 1);
		} else {
			emit_book(&lv->ent);
		}

		vpath[base_len] = '\0';
	}

	fs_closedir(&lv->dir);

	return 0;
}

static enum app_companion_status op_list(void)
{
	vpath[0] = '\0';

	return scan_dir(0) ? APP_COMPANION_STATUS_INTERNAL : APP_COMPANION_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* CHAPTERS                                                           */
/* ------------------------------------------------------------------ */

/* Copies book_id into vpath after checking it can only name a .npb below the
 * SD root: no leading slash, no hidden or ".." components, no backslashes (FatFs
 * treats them as separators). A full 128 byte id has no NUL, as LIST emits them.
 */
static bool set_vpath_from_book_id(const char *book_id)
{
	size_t len = strnlen(book_id, APP_COMPANION_BOOK_ID_LEN);
	const char *p;

	if (len == 0 || book_id[0] == '/' || memchr(book_id, '\\', len) != NULL) {
		return false;
	}

	vpath[0] = '/';
	memcpy(&vpath[1], book_id, len);
	vpath[1 + len] = '\0';

	for (p = vpath + 1; *p; p++) {
		if (p[-1] == '/' && (*p == '.' || *p == '/')) {
			return false;
		}
	}

	return has_book_ext(vpath + 1);
}

static enum app_companion_status op_chapters(void)
{
	uint32_t count;
	ssize_t n;
	int ret;

	if (!set_vpath_from_book_id(req.cmd.book_id)) {
		printk("library: CHAPTERS rejected book_id\n");
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}

	ret = open_book();
	if (ret == -ENOENT) {
		return APP_COMPANION_STATUS_NOT_FOUND;
	}
	if (ret == -EINVAL) {
		printk("library: %s is not a valid book\n", vpath + 1);
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}
	if (ret) {
		printk("library: open %s failed (%d)\n", vpath + 1, ret);
		return APP_COMPANION_STATUS_INTERNAL;
	}

	count = hdr.chapter_count;
	if (count == 0) {
		fs_close(&file);
		return APP_COMPANION_STATUS_OK;
	}

	if (hdr.chapter_table_offset == 0 || count > UINT16_MAX) {
		fs_close(&file);
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}

	ret = fs_seek(&file, hdr.chapter_table_offset, FS_SEEK_SET);
	if (ret) {
		fs_close(&file);
		return APP_COMPANION_STATUS_INTERNAL;
	}

	for (uint32_t i = 0; i < count && !req.err; i++) {
		n = fs_read(&file, &chapter, sizeof(chapter));
		if (n != sizeof(chapter)) {
			printk("library: chapter %u read failed (%d)\n", (unsigned int)i, (int)n);
			fs_close(&file);
			return n < 0 ? APP_COMPANION_STATUS_INTERNAL
				     : APP_COMPANION_STATUS_BAD_REQUEST;
		}

		memcpy(chapter_rec.title, chapter.title, sizeof(chapter_rec.title));
		chapter_rec.text_offset = chapter.text_offset;
		emit_frame(APP_COMPANION_FRAME_CHAPTER, &chapter_rec, sizeof(chapter_rec));
	}

	fs_close(&file);

	return APP_COMPANION_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* DELETE                                                             */
/* ------------------------------------------------------------------ */

/* Removes the bookmark sidecar first, then the book. If the book unlink then
 * fails the book survives and only the reading position is lost; the other
 * order could leave a stale sidecar that a re-added book of the same name would
 * pick up.
 */
static enum app_companion_status op_delete(void)
{
	struct fs_dirent *ent = &levels[0].ent; /* no scan is running, reuse its static */
	int ret;

	if (!set_vpath_from_book_id(req.cmd.book_id)) {
		printk("library: DELETE rejected book_id\n");
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}

	ret = app_sd_full_path(vpath, full_path, sizeof(full_path));
	if (ret) {
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}

	ret = fs_stat(full_path, ent);
	if (ret == -ENOENT) {
		return APP_COMPANION_STATUS_NOT_FOUND;
	}
	if (ret) {
		printk("library: stat %s failed (%d)\n", vpath + 1, ret);
		return APP_COMPANION_STATUS_INTERNAL;
	}
	if (ent->type != FS_DIR_ENTRY_FILE) {
		return APP_COMPANION_STATUS_BAD_REQUEST;
	}

	if (bookmark_full_path() == 0) {
		ret = fs_unlink(full_path);
		if (ret && ret != -ENOENT) {
			printk("library: unlink sidecar of %s failed (%d)\n", vpath + 1, ret);
			return APP_COMPANION_STATUS_INTERNAL;
		}
	}

	ret = app_sd_full_path(vpath, full_path, sizeof(full_path));
	if (ret == 0) {
		ret = fs_unlink(full_path);
	}
	if (ret) {
		printk("library: unlink %s failed (%d)\n", vpath + 1, ret);
		return APP_COMPANION_STATUS_INTERNAL;
	}

	printk("library: deleted %s\n", vpath + 1);

	return APP_COMPANION_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* Request execution                                                  */
/* ------------------------------------------------------------------ */

static void lib_work_handler(struct k_work *work)
{
	enum app_companion_status status;
	uint8_t op = req.cmd.op;
	bool sd_up = false;
	uint32_t free_kb;
	uint32_t total_kb;
	int ret;

	ARG_UNUSED(work);

	app_power_mark_activity();
	printk("library: request op %u\n", op);

	if (app_usb_msc_is_active()) {
		status = APP_COMPANION_STATUS_STORAGE_BUSY;
		goto done;
	}

	ret = app_sd_init();
	if (ret == -EBUSY) {
		status = APP_COMPANION_STATUS_STORAGE_BUSY;
		goto done;
	}
	if (ret) {
		printk("library: SD init failed (%d)\n", ret);
		status = APP_COMPANION_STATUS_INTERNAL;
		goto done;
	}
	sd_up = true;

	switch (op) {
	case APP_COMPANION_LIB_LIST:
		status = op_list();
		break;
	case APP_COMPANION_LIB_CHAPTERS:
		status = op_chapters();
		break;
	case APP_COMPANION_LIB_DELETE:
		status = op_delete();
		break;
	default:
		status = APP_COMPANION_STATUS_BAD_REQUEST;
		break;
	}

	/* Still mounted here; after DELETE this picks up the freed space. */
	if (app_sd_get_usage(&free_kb, &total_kb) == 0) {
		k_spinlock_key_t key = k_spin_lock(&usage_lock);

		usage_free_kb = free_kb;
		usage_total_kb = total_kb;
		k_spin_unlock(&usage_lock, key);
	}

done:
	/* Release the card before the END frame, which can block on the phone. */
	if (sd_up) {
		app_sd_deinit();
	}

	emit_end(op, status);

	if (req.err) {
		printk("library: aborted, send failed (%d)\n", req.err);
	}

	/* conn must not be touched after this. */
	bt_conn_unref(req.conn);
	req.conn = NULL;
	atomic_clear(&busy);
}

bool app_companion_lib_get_sd_usage(uint32_t *free_kb, uint32_t *total_kb)
{
	k_spinlock_key_t key = k_spin_lock(&usage_lock);

	*free_kb = usage_free_kb;
	*total_kb = usage_total_kb;
	k_spin_unlock(&usage_lock, key);

	return *total_kb != APP_COMPANION_SD_SIZE_UNKNOWN;
}

int app_companion_lib_submit(struct bt_conn *conn, const void *cmd, uint16_t len,
			     app_companion_lib_send_t send)
{
	const uint8_t *bytes = cmd;
	struct bt_conn *ref;

	if (len == 0 || len > sizeof(req.cmd)) {
		return -EINVAL;
	}

	switch (bytes[0]) {
	case APP_COMPANION_LIB_LIST:
		if (len != 1 && len != sizeof(struct app_companion_lib_cmd)) {
			return -EINVAL;
		}
		break;
	case APP_COMPANION_LIB_CHAPTERS:
	case APP_COMPANION_LIB_DELETE:
		if (len != sizeof(struct app_companion_lib_cmd)) {
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	if (!atomic_cas(&busy, 0, 1)) {
		return -EBUSY;
	}

	ref = bt_conn_ref(conn);
	if (ref == NULL) {
		atomic_clear(&busy);
		return -ENOTCONN;
	}

	memset(&req.cmd, 0, sizeof(req.cmd));
	memcpy(&req.cmd, cmd, len);
	req.conn = ref;
	req.send = send;
	req.stage_len = 0;
	req.err = 0;

	k_work_submit_to_queue(&lib_workq, &lib_work);

	return 0;
}

static int lib_init(void)
{
	k_work_init(&lib_work, lib_work_handler);
	k_work_queue_init(&lib_workq);
	k_work_queue_start(&lib_workq, lib_stack, K_THREAD_STACK_SIZEOF(lib_stack), LIB_PRIORITY,
			   NULL);
	k_thread_name_set(&lib_workq.thread, "companion_lib");

	return 0;
}
SYS_INIT(lib_init, APPLICATION, 90);
