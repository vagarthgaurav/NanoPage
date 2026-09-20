/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_LIST_WIDGET_H_
#define APP_LIST_WIDGET_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Shared cursor/scroll behavior for the vertical entry lists used by the
 * browser, TOC, bookmarks, app menu, and reader menu screens.
 *
 * The widget owns cursor position and scroll offset; the caller owns the
 * entry data and is responsible for rendering a single row via draw_item().
 * draw_item() is called with an absolute grid row and must blank the row
 * itself when idx is out of range (i.e. no entry exists at that visible
 * slot) - this mirrors what every current screen already does.
 */
struct app_list_widget {
	void (*draw_item)(uint16_t row, size_t idx, void *user_data);
	void *user_data;

	uint16_t cursor_col;
	uint16_t content_start_row;
	uint16_t content_rows;

	size_t entry_count;
	size_t selected_index;
	size_t scroll_offset;
};

void app_list_widget_init(struct app_list_widget *widget,
			   void (*draw_item)(uint16_t row, size_t idx, void *user_data),
			   void *user_data, uint16_t cursor_col, uint16_t content_start_row,
			   uint16_t content_rows);

/* Sets the entry count and resets the cursor/scroll to the top of the list. */
void app_list_widget_reset(struct app_list_widget *widget, size_t entry_count);

/* Draws every visible row plus the cursor. Callers still own full/partial
 * display refresh cadence and must flush afterward.
 */
void app_list_widget_redraw(struct app_list_widget *widget);

/* Moves the cursor by delta (-1 or +1), scrolling the page if needed.
 * Redraws only what changed; callers still own the flush.
 */
void app_list_widget_move_cursor(struct app_list_widget *widget, int delta);

size_t app_list_widget_selected_index(const struct app_list_widget *widget);

#endif /* APP_LIST_WIDGET_H_ */
