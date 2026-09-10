/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>

#include "rtc.h"
#include "display.h"

int main(void)
{
	printk("NanoPage: RTC + buttons + display bring-up.\n");

	if (app_rtc_init()) {
		return 0;
	}

	app_display_init();

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
