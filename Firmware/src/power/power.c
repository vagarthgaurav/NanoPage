/* SPDX-License-Identifier: Apache-2.0 */

#include "power.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/util.h>

#include <hal/nrf_power.h>

#include "config.h"
#include "display.h"
#include "logo_nanopage.h"
#include "rtc.h"
#include "sd.h"

/*
 * APP_POWER_IDLE_TIMEOUT_MS (config.h): enforced by idle_work, a
 * k_work_delayable rescheduled to fire exactly APP_POWER_IDLE_TIMEOUT_MS
 * out on every app_power_mark_activity() call (buttons.c, usb_msc.c) -
 * not polled from some other subsystem's own cadence, so there's no lag
 * from a periodic tick to account for. Backed by the system workqueue's
 * timeout, which itself runs off the same 32.768kHz LFXO already clocking
 * the kernel's tickless idle - no extra I2C/SPI traffic, no extra power,
 * just exact timing instead of the old up-to-30s-plus-overhead slop.
 */

/*
 * Wake sources for System OFF. gpio-keys/the input subsystem owns these
 * pins during normal operation with edge-triggered interrupts, but that
 * config is irrelevant once sys_poweroff() runs - nothing survives to
 * dispatch an input event. What actually latches the SoC's wake logic is
 * reprogramming each pin for GPIO_INT_LEVEL_ACTIVE right before power-off,
 * done directly against the same underlying gpio0 controller.
 */
static const struct gpio_dt_spec wake_buttons[] = {
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_action), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_next), gpios),
	GPIO_DT_SPEC_GET(DT_ALIAS(sw_prev), gpios),
};

static struct k_work_delayable idle_work;
static bool woke_from_system_off;
static bool sleep_inhibited;
static void (*idle_hook)(void);

static void idle_work_handler(struct k_work *work);

/*
 * Direct NRF_POWER->RESETREAS read, not Zephyr's hwinfo subsystem - hwinfo's
 * nRF driver (HWINFO_NRF) only builds for NRF54HX or TF-M-secure targets in
 * this Zephyr version, not plain nRF52840. RTT doesn't survive a real power
 * event (see .claude/CLAUDE.md's "Environment quirks" note from the fuel
 * gauge POR work), so this is the one reliable way to confirm on hardware
 * that a boot was actually a System OFF wake, not a reset-button press or a
 * reflash.
 */
static void print_and_clear_reset_cause(void)
{
	uint32_t reason = nrf_power_resetreas_get(NRF_POWER);

	woke_from_system_off = (reason & NRF_POWER_RESETREAS_OFF_MASK) != 0;

	printk("power: RESETREAS=0x%08x%s\n", reason,
	       woke_from_system_off ? " (woke from System OFF via GPIO)" : "");

	nrf_power_resetreas_clear(NRF_POWER, reason);
}

void app_power_init(void)
{
	print_and_clear_reset_cause();
	k_work_init_delayable(&idle_work, idle_work_handler);
	k_work_reschedule(&idle_work, K_MSEC(APP_POWER_IDLE_TIMEOUT_MS));
}

void app_power_mark_activity(void)
{
	/*
	 * Mirrors app_power_check_idle()'s old sleep_inhibited gate: while
	 * inhibited, activity must not re-arm the idle timer, or a button
	 * press mid-USB-session would schedule a System OFF that fires the
	 * instant the inhibit is later lifted. app_power_set_sleep_inhibited()
	 * cancels any pending idle_work up front for the same reason; the
	 * caller lifting the inhibit is expected to call this again once
	 * sleep_inhibited is back to false (see usb_msc.c's release_session()).
	 */
	if (sleep_inhibited) {
		return;
	}

	k_work_reschedule(&idle_work, K_MSEC(APP_POWER_IDLE_TIMEOUT_MS));
}

/*
 * Shown as the panel's last image before power actually cuts - e-paper
 * holds it with no power draw, so this is what the screen displays for
 * the whole System OFF duration, not just a transient message. Draws the
 * pre-rendered NanoPage wordmark (logo_nanopage.h) straight to the panel
 * via app_display_refresh_full() - bypassing text.c's grid entirely, since
 * this is a full-screen bitmap, not character cells - which is also
 * already a full/ghost-free refresh, so no separate full-refresh call is
 * needed on top.
 */
static void show_sleep_message(void)
{
	app_display_refresh_full(app_logo_nanopage, sizeof(app_logo_nanopage));
}

static void enter_system_off(void)
{
	printk("power: %d minutes idle, entering System OFF (wake on any button)\n",
	       APP_POWER_IDLE_TIMEOUT_MS / 60000);

	/*
	 * Give whatever screen registered app_power_set_idle_hook() a chance
	 * to flush any state it hasn't already persisted, before the SD
	 * card/display below are touched at all - see that function's own
	 * doc comment.
	 */
	if (idle_hook) {
		idle_hook();
	}

	show_sleep_message();

	/*
	 * The PCF85063 keeps ticking through System OFF off its own always-on
	 * rail, independent of the MCU. main.c arms a once-a-minute alarm on
	 * it (RTC_ALARM_TIME_MASK_SECOND) whose INT1 line only ever gets
	 * cleared by the system-workqueue callback in rtc.c - which can't run
	 * once the MCU is powered off. Left armed, the next per-minute match
	 * latches INT1 low for the rest of System OFF (open-drain, no MCU
	 * left to clear AF), and R14's 10k pull-up then sources a continuous
	 * ~330uA for however long remains - confirmed on a PPK2 capture
	 * 2026-09-14 as a clean, delayed step from ~25uA to ~326uA with no
	 * boot-transient shape, well after entering sleep. Disarming here
	 * avoids that; app_rtc_set_alarm() re-arms it unconditionally on the
	 * next boot (main.c), since a System OFF wake is a full reboot.
	 */
	app_rtc_clear_alarm();

	/*
	 * The EPD panel's VCI is on the always-on 3.3V rail, not gated by
	 * the nRF52's own power state - left alone, it keeps burning its
	 * ~20-61uA post-refresh resting current for the entire System OFF
	 * duration. Deep sleep drops that to ~1uA. See display.c's comment
	 * on SSD1680_DEEP_SLEEP_MODE_1 for why this uses Mode 1, not the
	 * seemingly-better Mode 2: Mode 2 was tried first (2026-09-14) and
	 * confirmed on hardware to not reliably recover from a real System
	 * OFF wake (wake-time full refresh silently corrupted, BUSY
	 * clearing in ~230ms instead of ~1774ms) despite the datasheet
	 * describing both modes identically - Mode 1 doesn't have that
	 * problem, confirmed on hardware the same day. Safe to call here
	 * specifically because the next boot's app_display_init() does a
	 * fresh hardware reset as part of normal init - see the comment on
	 * app_display_deep_sleep() for why this isn't safe to call anywhere
	 * that doesn't lead to a full reboot.
	 */
	app_display_deep_sleep();

	/*
	 * Cut the microSD card's power here too, alongside the display -
	 * confirmed on hardware that leaving the card powered *off* while
	 * still inserted corrupts the shared spi3 bus for the display (see
	 * sd.h), so it can only be safely powered down once nothing is
	 * going to touch that bus again before the next reboot - exactly
	 * the case here, moments before sys_poweroff(). No-op if the card
	 * was never successfully powered on in the first place.
	 */
	app_sd_deinit();

	for (size_t i = 0; i < ARRAY_SIZE(wake_buttons); i++) {
		gpio_pin_configure_dt(&wake_buttons[i], GPIO_INPUT);
		gpio_pin_interrupt_configure_dt(&wake_buttons[i], GPIO_INT_LEVEL_ACTIVE);
	}

	sys_poweroff();
}

static void idle_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/*
	 * Defensive, not the primary gate (that's app_power_mark_activity()
	 * refusing to re-arm while inhibited, plus the cancel below on
	 * entering an inhibit) - only matters if idle_work is already
	 * running by the moment app_power_set_sleep_inhibited(true) tries to
	 * cancel it.
	 */
	if (sleep_inhibited) {
		return;
	}

	enter_system_off();
}

void app_power_set_sleep_inhibited(bool inhibited)
{
	sleep_inhibited = inhibited;

	if (inhibited) {
		k_work_cancel_delayable(&idle_work);
	}
}

void app_power_set_idle_hook(void (*cb)(void))
{
	idle_hook = cb;
}

bool app_power_woke_from_system_off(void)
{
	return woke_from_system_off;
}
