/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>

#include "rtc.h"

int main(void)
{
	printk("NanoPage: RTC + buttons bring-up.\n");

	if (app_rtc_init()) {
		return 0;
	}

	while (1) {
		app_rtc_log_time();
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
