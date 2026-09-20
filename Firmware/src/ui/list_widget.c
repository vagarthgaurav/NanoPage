/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>

#include "font_siji.h"
#include "list_widget.h"
#include "text.h"

static void draw_cursor(const struct app_list_widget *widget, uint16_t page_row, bool show)
{
	uint16_t row = widget->content_start_row + page_row;

	if (show) {
		app_text_draw_icon(row, app_text_col_to_px(widget->cursor_col),
				    APP_TEXT_ICON_CURSOR);
	} else {
		app_text_draw_cell(row, widget->cursor_col, ' ');
		app_text_draw_cell(row, widget->cursor_col + 1, ' ');
	}
}

void app_list_widget_init(struct app_list_widget *widget,
			   void (*draw_item)(uint16_t row, size_t idx, void *user_data),
			   void *user_data, uint16_t cursor_col, uint16_t content_start_row,
			   uint16_t content_rows)
{
	widget->draw_item = draw_item;
	widget->user_data = user_data;
	widget->cursor_col = cursor_col;
	widget->content_start_row = content_start_row;
	widget->content_rows = content_rows;
	widget->entry_count = 0;
	widget->selected_index = 0;
	widget->scroll_offset = 0;
}

void app_list_widget_reset(struct app_list_widget *widget, size_t entry_count)
{
	widget->entry_count = entry_count;
	widget->selected_index = 0;
	widget->scroll_offset = 0;
}

void app_list_widget_redraw(struct app_list_widget *widget)
{
	for (uint16_t i = 0; i < widget->content_rows; i++) {
		widget->draw_item(widget->content_start_row + i, widget->scroll_offset + i,
				   widget->user_data);
	}

	if (widget->entry_count > 0) {
		draw_cursor(widget, (uint16_t)(widget->selected_index - widget->scroll_offset),
			    true);
	}
}

void app_list_widget_move_cursor(struct app_list_widget *widget, int delta)
{
	size_t new_index;
	uint16_t old_page_row;

	if (widget->entry_count == 0) {
		return;
	}

	if (delta < 0) {
		if (widget->selected_index == 0) {
			return;
		}
		new_index = widget->selected_index - 1;
	} else {
		if (widget->selected_index + 1 >= widget->entry_count) {
			return;
		}
		new_index = widget->selected_index + 1;
	}

	if (new_index < widget->scroll_offset ||
	    new_index >= widget->scroll_offset + widget->content_rows) {
		widget->scroll_offset = (new_index < widget->scroll_offset)
						 ? new_index
						 : new_index - widget->content_rows + 1;
		widget->selected_index = new_index;
		app_list_widget_redraw(widget);
	} else {
		old_page_row = (uint16_t)(widget->selected_index - widget->scroll_offset);
		draw_cursor(widget, old_page_row, false);
		widget->selected_index = new_index;
		draw_cursor(widget, (uint16_t)(widget->selected_index - widget->scroll_offset),
			    true);
	}
}

size_t app_list_widget_selected_index(const struct app_list_widget *widget)
{
	return widget->selected_index;
}
