/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_USB_MSC_H_
#define APP_USB_MSC_H_

#include <stdbool.h>

/*
 * USB Mass Storage: exposes the microSD card as a plain USB drive whenever
 * a host is connected, so books can be dragged onto the card without
 * pulling it out of the board. Built on the nRF52840's native USBD
 * peripheral (VBUS is wired straight to it - see the README pinout) and
 * Zephyr's newer USB device stack (CONFIG_USB_DEVICE_STACK_NEXT)'s
 * USBD_MSC_CLASS, which serves the SD card's raw disk_access block device
 * directly to the host - no FAT mount on this side, so the on-disk
 * filesystem only ever has one writer (the host) at a time. See
 * app_sd_usb_claim()/app_sd_usb_release() in sd.h for how this and the
 * rest of the app (browser.c/reader.c/currentlog.c) stay out of each
 * other's way for the shared regulator/CS/bus state.
 *
 * usbd_init() runs once at app_usb_msc_init() regardless of whether a
 * cable happens to be plugged in yet; usbd_enable()/usbd_disable() are
 * driven entirely by the controller's own VBUS_READY/VBUS_REMOVED
 * messages instead (nRF52840's UDC driver can detect VBUS natively - see
 * can_detect_vbus in udc_nrf.c - so this is the stack's own documented
 * pattern for a self-detecting controller). Those messages run on the
 * system workqueue (CONFIG_USBD_MSG_DEFERRED_MODE, the default), the same
 * thread every other event source in this app already funnels through
 * (RTC alarm, charger STAT, the footer's periodic tick) - safe to do
 * blocking display/SD work directly from there, same as charger.c/rtc.c.
 *
 * While a host is connected: takes over button input (Prev/Next/Action do
 * nothing - there's no local UI action that makes sense while the host
 * owns the card), draws a "connected" message into the content area
 * (header/footer keep ticking as normal - they don't touch the SD card),
 * and tells power.c to hold off System OFF (cutting SD/display power
 * mid-transfer would corrupt whatever the host was writing). On
 * disconnect, hands the card back and re-opens the file browser at
 * whatever folder was showing before, so anything the host just added,
 * removed, or renamed shows up immediately without a manual refresh.
 */

/*
 * Sets up the USB device stack (descriptors, the MSC class, the SD card
 * as its one LUN) and registers for VBUS notifications. Call once from
 * main(), after app_text_init()/app_buttons_init() - the "connected"
 * screen needs the text grid ready, and taking over button input needs
 * buttons.c ready. Does not attach to the bus (no D+ pull-up) until a
 * real VBUS-ready event arrives, so it's safe to call this whether or
 * not a cable happens to already be plugged in at boot.
 *
 * Returns 0 on success, negative errno if the USB device stack failed to
 * initialize.
 */
int app_usb_msc_init(void);

/*
 * True from the moment a host connection is detected until it's released -
 * for main.c's header to show a "connected" indicator instead of the
 * current folder path (see on_header_event() in main.c).
 */
bool app_usb_msc_is_active(void);

#endif /* APP_USB_MSC_H_ */
