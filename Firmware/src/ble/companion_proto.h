/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Wire format of the NanoPage companion BLE protocol. Single source of truth
 * for the firmware; mirrored by hand in the phone app repo. Bump
 * APP_COMPANION_PROTO_VERSION on any change. Spec: docs/companion-protocol.md.
 *
 * All integers little-endian. Every struct is packed and byte-for-byte what
 * goes on the air.
 */

#ifndef APP_COMPANION_PROTO_H_
#define APP_COMPANION_PROTO_H_

#include <stdint.h>

#define APP_COMPANION_PROTO_VERSION 1

/*
 * 128-bit UUIDs. Service and characteristics share one base; only the first
 * 32-bit group differs.
 */

/* Textual forms, for the app side and for logs. */
#define APP_COMPANION_UUID_SERVICE_STR   "5412c04e-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_DEVICE_INFO_STR "5412c051-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_TIME_STR      "5412c052-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_LIB_CTRL_STR  "5412c053-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_LIB_DATA_STR  "5412c054-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_CAL_CTRL_STR  "5412c055-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_CAL_DATA_STR  "5412c056-5068-470a-8d10-96f9301ba429"
#define APP_COMPANION_UUID_CAL_STATUS_STR "5412c057-5068-470a-8d10-96f9301ba429"

/* Same UUIDs as BT_UUID_128_ENCODE() arguments: (w32, w1, w2, w3, w48). */
#define APP_COMPANION_UUID_SERVICE_VAL     0x5412c04e, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_DEVICE_INFO_VAL 0x5412c051, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_TIME_VAL        0x5412c052, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_LIB_CTRL_VAL    0x5412c053, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_LIB_DATA_VAL    0x5412c054, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_CAL_CTRL_VAL    0x5412c055, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_CAL_DATA_VAL    0x5412c056, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL
#define APP_COMPANION_UUID_CAL_STATUS_VAL  0x5412c057, 0x5068, 0x470a, 0x8d10, 0x96f9301ba429ULL

/* Advertised local name is "NanoPage " + last 4 hex digits of the device id. */
#define APP_COMPANION_NAME_PREFIX "NanoPage "

enum app_companion_status {
	APP_COMPANION_STATUS_OK = 0,
	APP_COMPANION_STATUS_BAD_REQUEST = 1,
	APP_COMPANION_STATUS_STORAGE_BUSY = 2, /* USB MSC or display owns the SD bus */
	APP_COMPANION_STATUS_CRC_MISMATCH = 3,
	APP_COMPANION_STATUS_NOT_FOUND = 4,
	APP_COMPANION_STATUS_TOO_LARGE = 5,
	APP_COMPANION_STATUS_INTERNAL = 6,
};

/* --- DEVICE_INFO (read) --- */

#define APP_COMPANION_FW_VERSION_LEN 16
#define APP_COMPANION_SD_SIZE_UNKNOWN 0xFFFFFFFFu

struct app_companion_device_info {
	uint8_t proto_version;
	char fw_version[APP_COMPANION_FW_VERSION_LEN]; /* NUL padded */
	uint8_t battery_pct;                           /* 0..100, 0xFF unknown */
	uint8_t charging;                              /* 0 / 1 */
	uint32_t sd_free_kb;                           /* APP_COMPANION_SD_SIZE_UNKNOWN if n/a */
	uint32_t sd_total_kb;
} __packed;

/* --- TIME (write) --- local wall-clock, sets the RTC --- */

struct app_companion_time {
	uint16_t year; /* e.g. 2026 */
	uint8_t month; /* 1..12 */
	uint8_t day;   /* 1..31 */
	uint8_t hour;  /* 0..23 */
	uint8_t min;   /* 0..59 */
	uint8_t sec;   /* 0..59 */
} __packed;

/* --- LIB_CTRL (write) --- */

#define APP_COMPANION_BOOK_ID_LEN 64 /* path relative to the SD root, NUL padded */

enum app_companion_lib_op {
	APP_COMPANION_LIB_LIST = 0x01,     /* no args */
	APP_COMPANION_LIB_CHAPTERS = 0x02, /* book_id[64] */
	APP_COMPANION_LIB_DELETE = 0x03,   /* book_id[64] */
};

struct app_companion_lib_cmd {
	uint8_t op;
	char book_id[APP_COMPANION_BOOK_ID_LEN]; /* ignored for LIST */
} __packed;

/* --- LIB_DATA (notify) ---
 *
 * A byte stream of frames, concatenated across notifications; a frame may
 * span notifications, so the receiver reassembles:
 *
 *   frame = type u8, len u16, payload[len]
 *
 * A response is zero or more RECORD frames followed by exactly one END frame.
 */

enum app_companion_frame_type {
	APP_COMPANION_FRAME_BOOK = 0x01,    /* payload: app_companion_book_record */
	APP_COMPANION_FRAME_CHAPTER = 0x02, /* payload: app_companion_chapter_record */
	APP_COMPANION_FRAME_END = 0x7F,     /* payload: app_companion_end_frame */
};

struct app_companion_frame_hdr {
	uint8_t type;
	uint16_t len;
} __packed;

#define APP_COMPANION_TITLE_LEN 64
#define APP_COMPANION_AUTHOR_LEN 64

struct app_companion_book_record {
	char book_id[APP_COMPANION_BOOK_ID_LEN];
	char title[APP_COMPANION_TITLE_LEN];
	char author[APP_COMPANION_AUTHOR_LEN];
	uint32_t file_size;
	uint32_t text_blob_size;
	uint32_t last_position;
	uint16_t chapter_count;
	uint32_t last_read_local; /* minutes since 2000-01-01 00:00 local, 0 unknown */
} __packed;

struct app_companion_chapter_record {
	char title[APP_COMPANION_TITLE_LEN];
	uint32_t text_offset;
} __packed;

struct app_companion_end_frame {
	uint8_t op;     /* the lib_op / cal_op this answers */
	uint8_t status; /* enum app_companion_status */
} __packed;

/* --- CAL_CTRL (write) / CAL_STATUS (notify) --- */

enum app_companion_cal_op {
	APP_COMPANION_CAL_BEGIN = 0x01,  /* app_companion_cal_begin */
	APP_COMPANION_CAL_COMMIT = 0x02, /* no args */
	APP_COMPANION_CAL_ABORT = 0x03,  /* no args */
};

struct app_companion_cal_begin {
	uint8_t op; /* APP_COMPANION_CAL_BEGIN */
	uint32_t total_len;
	uint32_t crc32; /* IEEE CRC-32 (same as zlib/crc32_ieee) of the whole blob */
	uint16_t event_count;
} __packed;

/*
 * CAL_STATUS notifies the outcome of BEGIN and COMMIT (a CAL_CTRL write that
 * is merely malformed fails with an ATT error instead).
 */
struct app_companion_cal_status {
	uint8_t op;
	uint8_t status; /* enum app_companion_status */
} __packed;

/* --- Calendar blob (written through CAL_DATA, chunked by the app) --- */

#define APP_COMPANION_CAL_MAGIC "NPCL"
#define APP_COMPANION_CAL_VERSION 1
#define APP_COMPANION_CAL_ALL_DAY_DURATION 0xFFFF
#define APP_COMPANION_CAL_FLAG_ALL_DAY 0x01

struct app_companion_cal_header {
	char magic[4];
	uint16_t version;
	uint16_t event_count;
	uint32_t range_start_local; /* minutes since 2000-01-01 00:00 local */
} __packed;

struct app_companion_cal_event {
	uint32_t start_local; /* minutes since 2000-01-01 00:00 local */
	uint16_t duration_min;
	uint8_t flags;
	uint8_t title_len;
	/* char title[title_len] follows, 7-bit ASCII 32..126 */
} __packed;

#endif /* APP_COMPANION_PROTO_H_ */
