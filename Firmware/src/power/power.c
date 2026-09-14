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
#include "sd.h"
#include "text.h"

/*
 * APP_POWER_IDLE_TIMEOUT_MS (config.h): checked from whatever cadence
 * app_power_check_idle() is called at (currently main.c's 30s
 * status-refresh tick), so this is a floor, not a precise deadline - fine
 * given the 30s granularity is <0.2% of the timeout.
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

static int64_t last_activity_ms;
static bool woke_from_system_off;

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
	last_activity_ms = k_uptime_get();
}

void app_power_mark_activity(void)
{
	last_activity_ms = k_uptime_get();
}

/*
 * Shown as the panel's last image before power actually cuts - e-paper
 * holds it with no power draw, so this is what the screen displays for
 * the whole System OFF duration, not just a transient message. A full
 * refresh (not fast/partial) so it's ghost-free no matter what was on
 * screen before.
 */
static void show_sleep_message(void)
{
	uint16_t cols, rows;

	app_text_get_grid(&cols, &rows);
	app_text_clear();
	app_text_draw_row(rows / 2 - 1, "Sleeping...");
	app_text_draw_row(rows / 2, "press any button to wake");
	app_text_full_refresh();
}

static void enter_system_off(void)
{
	printk("power: %d minutes idle, entering System OFF (wake on any button)\n",
	       APP_POWER_IDLE_TIMEOUT_MS / 60000);

	show_sleep_message();

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

void app_power_check_idle(void)
{
	if (k_uptime_get() - last_activity_ms >= APP_POWER_IDLE_TIMEOUT_MS) {
		enter_system_off();
	}
}
