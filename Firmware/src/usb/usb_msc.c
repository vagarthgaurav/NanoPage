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

/*
 * pid.codes (http://pid.codes) is a shared Vendor ID for open-source
 * hardware projects that haven't purchased their own from usb.org - PID
 * 0x0001 under it is pid.codes' own reserved "test PID", explicitly not
 * for public distribution. Fine for this one-off personal board; get a
 * real allocated PID from pid.codes (or buy a private VID) first if
 * NanoPage is ever built for anyone else.
 */
#define NANOPAGE_USBD_VID 0x1209
#define NANOPAGE_USBD_PID 0x0001

/* USB spec units: bMaxPower is in 2mA steps, so 50 -> 100mA. The board is
 * self-powered (battery) rather than relying on the bus, but still worth
 * declaring a modest bus-power allowance for spec compliance.
 */
#define NANOPAGE_USBD_MAX_POWER 50

USBD_DEVICE_DEFINE(nanopage_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)), NANOPAGE_USBD_VID,
		    NANOPAGE_USBD_PID);

USBD_DESC_LANG_DEFINE(nanopage_lang);
USBD_DESC_MANUFACTURER_DEFINE(nanopage_mfr, "NanoPage");
USBD_DESC_PRODUCT_DEFINE(nanopage_product, "NanoPage E-Reader");
USBD_DESC_CONFIG_DEFINE(nanopage_cfg_desc, "MSC");

USBD_CONFIGURATION_DEFINE(nanopage_fs_config, USB_SCD_SELF_POWERED, NANOPAGE_USBD_MAX_POWER,
			   &nanopage_cfg_desc);

/* Disk name must match APP_SD_DISK_NAME (sd.h) - the same disk_access
 * block device app_sd_usb_claim()/app_sd_usb_release() initialize/tear
 * down around a connection.
 */
USBD_DEFINE_MSC_LUN(sd, APP_SD_DISK_NAME, "NanoPage", "SD Card", "0.00");

static bool msc_active;
static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;

/*
 * Blanks the content area, then draws `lines` vertically centered within
 * it, each one horizontally centered too - same content-area convention
 * (between the header and footer rows) browser.c/reader.c use, just with
 * a fixed static message instead of live content.
 */
static void draw_message(const char *const *lines, size_t line_count)
{
	uint16_t start_offset =
		(content_rows > line_count) ? (uint16_t)((content_rows - line_count) / 2) : 0;

	for (uint16_t i = 0; i < content_rows; i++) {
		app_text_draw_row((uint16_t)(content_start_row + i), "");
	}

	for (size_t i = 0; i < line_count && (start_offset + i) < content_rows; i++) {
		/* Every caller passes short, hardcoded strings well under
		 * this - not sized for arbitrary/untrusted input.
		 */
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
	/* There's no local UI action that makes sense while the host owns
	 * the card - button presses are swallowed, not forwarded anywhere.
	 */
}

/*
 * Claims the card/screen/buttons for an actual, enumerated USB Mass
 * Storage session - see the long comment on USBD_MSG_CONFIGURATION in
 * usb_msg_cb() below for why this is gated on real enumeration rather
 * than just VBUS being present.
 */
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

/* Reverses claim_session() - also reachable from a mid-connection
 * deconfigure (SET_CONFIGURATION(0)), not just a real disconnect, so it
 * doesn't touch usbd_enable()/usbd_disable() itself - see usb_msg_cb().
 */
static void release_session(void)
{
	if (!msc_active) {
		return;
	}

	app_sd_usb_release();
	msc_active = false;
	app_power_set_sleep_inhibited(false);
	app_power_mark_activity();

	/*
	 * claim_session() never touches browser.c's own state (it just
	 * swaps the button callback and draws over whatever the browser had
	 * drawn) - so app_browser_is_ready() still reflects whether the
	 * browser was the screen actually showing when USB connected, not
	 * just whether it's ever been opened this boot. If so, re-list
	 * whatever folder that was (the host may have added, removed, or
	 * renamed files while it had the card) and resume there. Otherwise
	 * (the app menu or settings screen was showing instead) return to
	 * the menu rather than jumping into the file browser unprompted -
	 * a real simplification for the settings-screen case specifically
	 * (it's not restored to exactly where it was), accepted since
	 * nothing there is lost by re-opening it from the menu again.
	 */
	if (app_browser_is_ready()) {
		if (app_browser_reinit() != 0) {
			/* The card that was there before is now unreadable
			 * (e.g. pulled out while it was mounted over USB) -
			 * fall back to the menu rather than a dead-end "no
			 * card" message with no button callback to leave it.
			 */
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

	/*
	 * Only attaches to the bus (D+ pull-up) - does NOT claim the card
	 * or touch the screen. VBUS is just "5V is present," true for a
	 * dumb USB charger as much as a real computer - see usb_msg_cb()'s
	 * USBD_MSG_CONFIGURATION handling for where the actual host-vs-
	 * charger distinction is made.
	 */
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
		/*
		 * Fires only on a real SET_CONFIGURATION request completing -
		 * i.e. a real USB host finished enumerating us and picked our
		 * one configuration (msg->status carries the value it chose,
		 * matching struct usb_setup_packet's wValue - see
		 * sreq_set_configuration() in usbd_ch9.c). A plain charger
		 * provides VBUS but never speaks USB at all, so it can never
		 * reach this - claiming the card/taking over the screen here
		 * instead of on VBUS_READY is what keeps a charger-only
		 * connection from showing "Connected to computer" (found
		 * 2026-09-18: the original VBUS_READY-gated version did
		 * exactly that, since a charger's VBUS looks identical to a
		 * real host's at that point in the sequence). wValue == 0 is
		 * the formal "deconfigure" request (rare with the cable still
		 * attached, but the host is allowed to do it) - treated the
		 * same as a disconnect for our purposes, just without the
		 * usbd_disable() a real VBUS_REMOVED also does.
		 */
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

	/* No serial-number descriptor: it needs CONFIG_HWINFO, and
	 * HWINFO_NRF only builds for NRF54H/TF-M-secure targets in this
	 * Zephyr version, not plain nRF52840 (confirmed against
	 * drivers/hwinfo/Kconfig.nrf) - not worth chasing for an optional
	 * string. Most hosts show the device fine without one.
	 */

	err = usbd_add_configuration(&nanopage_usbd, USBD_SPEED_FS, &nanopage_fs_config);
	if (err) {
		printk("usb_msc: add configuration failed (%d)\n", err);
		return err;
	}

	/* Only USBD_MSC_CLASS is compiled in (prj.conf), so this registers
	 * just the one MSC instance - no blocklist needed.
	 */
	err = usbd_register_all_classes(&nanopage_usbd, USBD_SPEED_FS, 1, NULL);
	if (err) {
		printk("usb_msc: register classes failed (%d)\n", err);
		return err;
	}

	/* Use the class's own interface descriptor for the device class
	 * triple (MSC is a single-interface class, not one of the
	 * IAD-needing multi-interface ones like CDC/UAC/UVC).
	 */
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
