/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_msc.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usbd_msg.h>

#include "browser.h"
#include "buttons.h"
#include "header.h"
#include "menu.h"
#include "power.h"
#include "sd.h"
#include "text.h"
#include "usb_msc.h"

#define NANOPAGE_USBD_VID 0x1209
#define NANOPAGE_USBD_PID 0x0001

#define NANOPAGE_USBD_MAX_POWER 50

USBD_DEVICE_DEFINE(nanopage_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), NANOPAGE_USBD_VID,
		    NANOPAGE_USBD_PID);

USBD_DESC_LANG_DEFINE(nanopage_lang);
USBD_DESC_MANUFACTURER_DEFINE(nanopage_mfr, "NanoPage");
USBD_DESC_PRODUCT_DEFINE(nanopage_product, "NanoPage E-Reader");
USBD_DESC_CONFIG_DEFINE(nanopage_cfg_desc, "MSC");

USBD_CONFIGURATION_DEFINE(nanopage_fs_config, USB_SCD_SELF_POWERED, NANOPAGE_USBD_MAX_POWER,
			   &nanopage_cfg_desc);

USBD_DEFINE_MSC_LUN(sd, APP_SD_DISK_NAME, "NanoPage", "SD Card", "0.00");

static bool msc_active;
static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

static void draw_message(const char *const *lines, size_t line_count)
{
	uint16_t start_offset =
		(content_rows > line_count) ? (uint16_t)((content_rows - line_count) / 2) : 0;

	for (uint16_t i = 0; i < content_rows; i++) {
		app_text_draw_row((uint16_t)(content_start_row + i), "");
	}

	for (size_t i = 0; i < line_count && (start_offset + i) < content_rows; i++) {
		char padded[64];
		size_t len = strlen(lines[i]);
		size_t pad = (len < grid_cols) ? (size_t)(grid_cols - len) / 2 : 0;

		if (pad + len >= sizeof(padded)) {
			pad = 0;
		}

		memset(padded, ' ', pad);
		strncpy(padded + pad, lines[i], sizeof(padded) - pad - 1);
		padded[sizeof(padded) - 1] = '\0';

		app_text_draw_row((uint16_t)(content_start_row + start_offset + i), padded);
	}
}

static void draw_connected_screen(void)
{
	static const char *const lines[] = {
		"Connected to computer",
		"",
		"Drag and drop files",
		"onto the SD card",
	};

	draw_message(lines, ARRAY_SIZE(lines));
	app_text_full_refresh();
}

static void ignore_button(enum app_button button)
{
	ARG_UNUSED(button);
}

static void claim_session(void)
{
	int ret;

	if (msc_active) {
		return;
	}

	msc_active = true;
	app_power_set_sleep_inhibited(true);

	ret = app_sd_usb_claim();
	if (ret) {
		printk("usb_msc: SD claim failed (%d) - exposing an empty drive\n", ret);
	}

	app_buttons_set_press_callback(ignore_button);
	draw_connected_screen();
}

static void release_session(void)
{
	if (!msc_active) {
		return;
	}

	app_sd_usb_release();
	msc_active = false;
	app_power_set_sleep_inhibited(false);
	app_power_mark_activity();

	if (app_browser_is_ready()) {
		if (app_browser_reinit() != 0) {
			printk("usb_msc: browser re-init failed after disconnect\n");
			app_menu_show();
		}
	} else {
		app_menu_show();
	}
}

static void handle_vbus_ready(void)
{
	int ret;

	ret = usbd_enable(&nanopage_usbd);
	if (ret) {
		printk("usb_msc: usbd_enable failed (%d)\n", ret);
	}
}

static void handle_vbus_removed(void)
{
	usbd_disable(&nanopage_usbd);
	release_session();
}

static void usb_msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	switch (msg->type) {
	case USBD_MSG_VBUS_READY:
		handle_vbus_ready();
		break;
	case USBD_MSG_VBUS_REMOVED:
		handle_vbus_removed();
		break;
	case USBD_MSG_CONFIGURATION:
		if (msg->status != 0) {
			claim_session();
		} else {
			release_session();
		}
		break;
	default:
		break;
	}
}

static int setup_usbd(void)
{
	int err;

	err = usbd_add_descriptor(&nanopage_usbd, &nanopage_lang);
	if (err) {
		printk("usb_msc: language descriptor failed (%d)\n", err);
		return err;
	}

	err = usbd_add_descriptor(&nanopage_usbd, &nanopage_mfr);
	if (err) {
		printk("usb_msc: manufacturer descriptor failed (%d)\n", err);
		return err;
	}

	err = usbd_add_descriptor(&nanopage_usbd, &nanopage_product);
	if (err) {
		printk("usb_msc: product descriptor failed (%d)\n", err);
		return err;
	}

	err = usbd_add_configuration(&nanopage_usbd, USBD_SPEED_FS, &nanopage_fs_config);
	if (err) {
		printk("usb_msc: add configuration failed (%d)\n", err);
		return err;
	}

	err = usbd_register_all_classes(&nanopage_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		printk("usb_msc: register classes failed (%d)\n", err);
		return err;
	}

	usbd_device_set_code_triple(&nanopage_usbd, USBD_SPEED_FS, 0, 0, 0);
	usbd_self_powered(&nanopage_usbd, true);

	err = usbd_msg_register_cb(&nanopage_usbd, usb_msg_cb);
	if (err) {
		printk("usb_msc: message callback registration failed (%d)\n", err);
		return err;
	}

	err = usbd_init(&nanopage_usbd);
	if (err) {
		printk("usb_msc: usbd_init failed (%d)\n", err);
		return err;
	}

	return 0;
}

int app_usb_msc_init(void)
{
	uint16_t rows;

	app_text_get_grid(&grid_cols, &rows);
	content_start_row = APP_HEADER_ROW + 1;
	content_rows = (rows >= 2) ? (uint16_t)(rows - 2) : 0;

	return setup_usbd();
}

bool app_usb_msc_is_active(void)
{
	return msc_active;
}
