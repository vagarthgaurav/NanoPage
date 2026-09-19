/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SD_H_
#define APP_SD_H_

#include <stdbool.h>
#include <stddef.h>

#define APP_SD_DISK_NAME "SD"

int app_sd_init(void);

void app_sd_deinit(void);

int app_sd_usb_claim(void);

void app_sd_usb_release(void);

int app_sd_power_on(void);
void app_sd_power_off(void);

#define APP_SD_MAX_PATH_LEN 200

#define APP_SD_MAX_NAME_LEN 48

struct app_sd_dirent {
	char name[APP_SD_MAX_NAME_LEN];
	bool is_dir;
};

int app_sd_list_dir(const char *path, struct app_sd_dirent *entries, size_t max_entries,
		     size_t *count);

int app_sd_full_path(const char *path, char *out, size_t out_size);

#endif /* APP_SD_H_ */
