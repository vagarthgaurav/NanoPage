/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_READER_H_
#define APP_READER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "npb_bookmark_format.h"
#include "text.h"

int app_reader_open(const char *path, void (*on_exit)(void));

int app_reader_resume_last_session(void (*on_exit)(void));

enum app_text_font app_reader_get_font(void);

const char *app_reader_get_path(void);

int app_reader_jump_to_offset(uint32_t text_offset);

void app_reader_set_font(enum app_text_font font);

bool app_reader_is_active(void);

bool app_reader_current_page_bookmarked(void);

uint16_t app_reader_get_content_rows(void);

int app_reader_get_bookmarks(struct npb_bookmark *out, size_t max, size_t *count);

#endif /* APP_READER_H_ */
