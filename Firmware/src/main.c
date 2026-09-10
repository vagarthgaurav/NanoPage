/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>

int main(void)
{
	unsigned int count = 0;

	printk("NanoPage: first flash over SWD, board is alive.\n");

	while (1) {
		printk("heartbeat %u\n", count++);
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
