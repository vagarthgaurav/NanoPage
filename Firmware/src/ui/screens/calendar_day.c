/* SPDX-License-Identifier: Apache-2.0 */

/*
 * MOCKUP: the events below are hardcoded fake data, keyed by the viewed day's
 * offset from today. Replace fill_day() (and the event_count()/event_at()
 * accessors) with a real event store.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/sys/util.h>

#include "buttons.h"
#include "calendar_day.h"
#include "header.h"
#include "list_widget.h"
#include "rtc.h"
#include "text.h"

#define APP_CALENDAR_CURSOR_COL  0
#define APP_CALENDAR_TIME_COL    2
#define APP_CALENDAR_TITLE_COL   8
#define APP_CALENDAR_DUR_END_COL 47
#define APP_CALENDAR_HINT_COL    48

#define APP_CALENDAR_LINE_MARGIN  4
#define APP_CALENDAR_LINE_CAP_END 12

#define APP_CALENDAR_MAX_EVENTS  32
#define APP_CALENDAR_MAX_ALL_DAY 4

#define APP_CALENDAR_EMPTY_MSG "Nothing planned"
#define APP_CALENDAR_EMPTY_ROW 2

#define APP_CALENDAR_FALLBACK_YEAR  2026
#define APP_CALENDAR_FALLBACK_MONTH 1
#define APP_CALENDAR_FALLBACK_DAY   1

#define HM(h, m) ((h) * 60 + (m))

struct cal_event {
	uint16_t start_min;
	uint16_t dur_min;
	const char *title;
};

static const char *const set0_all_day[] = {"Diwali", "Priya's birthday"};

static const struct cal_event set0_timed[] = {
	{HM(9, 0), 15, "Team standup"},
	{HM(10, 30), 45, "Dentist"},
	{HM(12, 30), 60, "Lunch w/ Priya"},
	{HM(15, 0), 60, "PCB review"},
	{HM(17, 0), 30, "Pick up parcel"},
	{HM(18, 30), 60, "Gym"},
	{HM(21, 0), 30, "Call Mom"},
};

static const struct cal_event set1_timed[] = {
	{HM(8, 30), 45, "Run"},
	{HM(10, 0), 60, "Sprint planning"},
	{HM(13, 0), 60, "Lunch w/ Arjun"},
	{HM(14, 30), 15, "Order stencil"},
	{HM(16, 0), 90, "Display review"},
	{HM(19, 0), 120, "Dinner"},
};

static const char *const weekday_name[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

/* Per-day state, refilled by fill_day() on every day change. */
static struct cal_event day_events[APP_CALENDAR_MAX_EVENTS];
static size_t day_event_count;
static const char *day_all_day[APP_CALENDAR_MAX_ALL_DAY];
static size_t day_all_day_count;

static struct app_list_widget list;

static void (*exit_cb)(void);
static uint16_t grid_cols;
static uint16_t content_start_row;
static uint16_t content_rows;
static bool calendar_active;

/* Days since 1970-01-01 for today, and the viewed day as an offset from it. */
static int32_t today_days;
static int day_offset;
/* Minute-of-day of "now", or -1 when the RTC could not be read. */
static int now_min;
/* Index of the first timed event starting at or after now (count if none). */
static size_t first_future;
static bool now_line;

/* Days since 1970-01-01 of a proleptic Gregorian date (m is 1-12). */
static int32_t days_from_civil(int y, unsigned int m, unsigned int d)
{
	y -= (m <= 2);

	int32_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned int yoe = (unsigned int)(y - era * 400);
	unsigned int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + (int32_t)doe - 719468;
}

static void civil_from_days(int32_t z, int *y, unsigned int *m, unsigned int *d)
{
	z += 719468;

	int32_t era = (z >= 0 ? z : z - 146096) / 146097;
	unsigned int doe = (unsigned int)(z - era * 146097);
	unsigned int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	unsigned int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	unsigned int mp = (5 * doy + 2) / 153;

	*d = doy - (153 * mp + 2) / 5 + 1;
	*m = mp < 10 ? mp + 3 : mp - 9;
	*y = (int)yoe + era * 400 + (*m <= 2);
}

static size_t event_count(void)
{
	return day_event_count;
}

static const struct cal_event *event_at(size_t idx)
{
	return &day_events[idx];
}

static void set_timed(const struct cal_event *src, size_t n)
{
	day_event_count = MIN(n, ARRAY_SIZE(day_events));
	memcpy(day_events, src, day_event_count * sizeof(day_events[0]));
}

/* Fills the per-day arrays for the day at the given offset from today. */
static void fill_day(int offset)
{
	int set = ((offset % 3) + 3) % 3;

	day_event_count = 0;
	day_all_day_count = 0;

	switch (set) {
	case 0:
		day_all_day_count = MIN(ARRAY_SIZE(set0_all_day), ARRAY_SIZE(day_all_day));
		for (size_t i = 0; i < day_all_day_count; i++) {
			day_all_day[i] = set0_all_day[i];
		}
		set_timed(set0_timed, ARRAY_SIZE(set0_timed));
		break;
	case 1:
		set_timed(set1_timed, ARRAY_SIZE(set1_timed));
		break;
	default:
		break;
	}
}

static void read_today(void)
{
	struct rtc_time now;

	if (app_rtc_get_time(&now) == 0) {
		today_days = days_from_civil(now.tm_year + 1900, (unsigned int)now.tm_mon + 1,
					     (unsigned int)now.tm_mday);
		now_min = now.tm_hour * 60 + now.tm_min;
	} else {
		today_days = days_from_civil(APP_CALENDAR_FALLBACK_YEAR, APP_CALENDAR_FALLBACK_MONTH,
					     APP_CALENDAR_FALLBACK_DAY);
		now_min = -1;
	}
}

int app_calendar_day_format_date(char *buf, size_t len)
{
	int32_t days = today_days + day_offset;
	unsigned int m, d;
	int y, n;

	civil_from_days(days, &y, &m, &d);
	n = snprintf(buf, len, "%s, %02u-%02u-%02d", weekday_name[((days % 7) + 7 + 4) % 7], m, d,
		     y % 100);

	return (n < 0 || (size_t)n >= len) ? -ENOSPC : 0;
}

static void update_header(void)
{
	char date[24];

	if (app_calendar_day_format_date(date, sizeof(date)) != 0) {
		snprintf(date, sizeof(date), "Calendar");
	}
	app_header_update(date, false);
}

static void draw_text_at(uint16_t row, uint16_t first_col, const char *str)
{
	size_t len = strlen(str);

	for (uint16_t col = first_col; col < grid_cols; col++) {
		size_t i = col - first_col;

		app_text_draw_cell(row, col, (i < len) ? str[i] : ' ');
	}
}

static void format_duration(char *buf, size_t len, unsigned int dur)
{
	if (dur == 0) {
		buf[0] = '\0';
	} else if (dur < 60) {
		snprintf(buf, len, "%um", dur);
	} else if (dur % 60 == 0) {
		snprintf(buf, len, "%uh", dur / 60);
	} else {
		snprintf(buf, len, "%uh%02u", dur / 60, dur % 60);
	}
}

static void draw_event(uint16_t row, size_t idx, void *user_data)
{
	const struct cal_event *ev;
	char buf[8];
	size_t len, title_max;
	uint16_t page_row = row - list.content_start_row;
	uint16_t dur_col;

	ARG_UNUSED(user_data);

	app_text_draw_row(row, "");

	if (idx >= event_count()) {
		return;
	}

	ev = event_at(idx);

	snprintf(buf, sizeof(buf), "%02u:%02u", ev->start_min / 60, ev->start_min % 60);
	for (uint16_t i = 0; buf[i] != '\0'; i++) {
		app_text_draw_cell(row, (uint16_t)(APP_CALENDAR_TIME_COL + i), buf[i]);
	}

	format_duration(buf, sizeof(buf), ev->dur_min);
	len = strlen(buf);
	dur_col = (uint16_t)(APP_CALENDAR_DUR_END_COL + 1 - len);
	for (uint16_t i = 0; i < len; i++) {
		app_text_draw_cell(row, (uint16_t)(dur_col + i), buf[i]);
	}

	/* Titles are cut off before the duration (one blank column), never wrapped. */
	title_max = dur_col - 1 - APP_CALENDAR_TITLE_COL;
	for (size_t i = 0; i < title_max && ev->title[i] != '\0'; i++) {
		app_text_draw_cell(row, (uint16_t)(APP_CALENDAR_TITLE_COL + i), ev->title[i]);
	}

	if (page_row == 0 && list.scroll_offset > 0) {
		app_text_draw_cell(row, APP_CALENDAR_HINT_COL, '^');
	}
	if (page_row == list.content_rows - 1 &&
	    list.scroll_offset + list.content_rows < event_count()) {
		app_text_draw_cell(row, APP_CALENDAR_HINT_COL, 'v');
	}
}

/*
 * The now line sits on the boundary between event first_future - 1 and
 * first_future. It is a thick 3px cap (x = MARGIN..CAP_END - 1, i.e. the
 * cursor columns) followed by a thin 1px rule up to MARGIN px short of the
 * right edge, centred on the cap. Tamzen 6x16 never inks pixel rows 0 and 15,
 * so between two rows the thin rule is y = 15 of the upper row and the cap is
 * y = 14..15 of the upper row plus y = 0 of the lower row. At the list edge
 * (only one side visible) everything goes inside the visible row: bottom edge
 * thin y = h-2, cap h-3..h-1 (rows 13-14 are blank); top edge thin y = 1, cap
 * 0..2 (only '$', '{' and '}' ink row 1). The cap covers just the cursor
 * columns, whose icon ink is in pixel rows 4-10, so it never touches text.
 *
 * It is drawn after the widget has drawn rows and cursor, because the cursor
 * cells overwrite the leftmost columns of their row.
 */
/* The thin rule is skipped when thin_y is negative. */
static void draw_line_rows(uint16_t row, int thin_y, uint16_t cap_y0, uint16_t cap_y1)
{
	uint16_t x_end = app_text_width_px() - APP_CALENDAR_LINE_MARGIN;

	if (thin_y >= 0) {
		app_text_draw_hline_span(row, (uint16_t)thin_y, APP_CALENDAR_LINE_CAP_END, x_end,
					 true);
	}
	for (uint16_t y = cap_y0; y <= cap_y1; y++) {
		app_text_draw_hline_span(row, y, APP_CALENDAR_LINE_MARGIN,
					 APP_CALENDAR_LINE_CAP_END, true);
	}
}

static void draw_now_line(void)
{
	size_t first = list.scroll_offset;
	size_t end = MIN(first + list.content_rows, event_count());
	uint16_t h = app_text_row_height();
	bool upper, lower;

	if (!now_line || event_count() == 0) {
		return;
	}

	upper = first_future > 0 && first_future - 1 >= first && first_future - 1 < end;
	lower = first_future < event_count() && first_future >= first && first_future < end;

	if (upper) {
		uint16_t row = (uint16_t)(list.content_start_row + (first_future - 1 - first));

		if (lower) {
			draw_line_rows(row, h - 1, h - 2, h - 1);
		} else {
			draw_line_rows(row, h - 2, h - 3, h - 1);
		}
	}
	if (lower) {
		uint16_t row = (uint16_t)(list.content_start_row + (first_future - first));

		if (upper) {
			draw_line_rows(row, -1, 0, 0);
		} else {
			draw_line_rows(row, 1, 0, 2);
		}
	}
}

static void draw_all_day_strip(uint16_t row)
{
	char buf[64];
	size_t pos = 0;

	app_text_draw_row(row, "");
	draw_text_at(row, APP_CALENDAR_TIME_COL, "ALL");

	buf[0] = '\0';
	for (size_t i = 0; i < day_all_day_count && pos < sizeof(buf); i++) {
		int n = snprintf(buf + pos, sizeof(buf) - pos, "%s%s", i ? ", " : "",
				 day_all_day[i]);

		if (n < 0) {
			break;
		}
		pos += n;
	}

	/* Cut off at the right edge of the grid, not wrapped. */
	draw_text_at(row, APP_CALENDAR_TITLE_COL, buf);
}

/* Fills the day, lays out strip + list and draws everything, no flush/refresh. */
static void render_day(void)
{
	size_t count, rows, max_scroll;
	uint16_t list_start = content_start_row;
	uint16_t list_rows = content_rows;
	bool today = (day_offset == 0);

	fill_day(day_offset);
	count = event_count();

	if (day_all_day_count > 0) {
		list_start++;
		list_rows--;
	}

	first_future = count;
	if (today && now_min >= 0) {
		for (size_t i = 0; i < count; i++) {
			if (event_at(i)->start_min >= now_min) {
				first_future = i;
				break;
			}
		}
	}
	now_line = today && now_min >= 0 && count > 0;

	app_list_widget_init(&list, draw_event, NULL, APP_CALENDAR_CURSOR_COL, list_start,
			      list_rows);
	app_list_widget_reset(&list, count);

	rows = list_rows;
	max_scroll = (count > rows) ? count - rows : 0;
	if (now_line && count > 0) {
		size_t scroll = first_future > 0 ? first_future - 1 : 0;

		list.selected_index = (first_future < count) ? first_future : count - 1;
		list.scroll_offset = MIN(scroll, max_scroll);
	}

	app_list_widget_redraw(&list);

	if (day_all_day_count > 0) {
		draw_all_day_strip(content_start_row);
	}

	if (count == 0) {
		draw_text_at((uint16_t)(content_start_row + APP_CALENDAR_EMPTY_ROW),
			     (uint16_t)((grid_cols - strlen(APP_CALENDAR_EMPTY_MSG)) / 2),
			     APP_CALENDAR_EMPTY_MSG);
	}

	draw_now_line();
}

static void refresh_day(void)
{
	render_day();
	update_header();
	app_text_full_refresh();
}

static bool is_line_row(size_t idx)
{
	return now_line && (idx == first_future || idx + 1 == first_future);
}

static void move_cursor(int delta)
{
	size_t old_idx = list.selected_index;
	size_t old_scroll = list.scroll_offset;

	if (event_count() == 0) {
		return;
	}

	app_list_widget_move_cursor(&list, delta);

	/* The cursor cells and a scroll redraw both wipe parts of the line. */
	if (list.scroll_offset != old_scroll || is_line_row(old_idx) ||
	    is_line_row(list.selected_index)) {
		draw_now_line();
	}

	app_text_flush();
}

static void change_day(int delta)
{
	day_offset += delta;
	refresh_day();
}

static void leave(void)
{
	void (*cb)(void) = exit_cb;

	exit_cb = NULL;
	calendar_active = false;
	app_buttons_set_press_callback(NULL);
	app_buttons_set_hold_callback(NULL);

	if (cb) {
		cb();
	}
}

static void on_button(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		move_cursor(-1);
		break;
	case APP_BUTTON_NEXT:
		move_cursor(1);
		break;
	case APP_BUTTON_ACTION:
		leave();
		break;
	}
}

static void on_hold(enum app_button button)
{
	switch (button) {
	case APP_BUTTON_PREV:
		change_day(-1);
		break;
	case APP_BUTTON_NEXT:
		change_day(1);
		break;
	default:
		break;
	}
}

void app_calendar_day_show(void (*on_exit)(void))
{
	uint16_t rows;

	exit_cb = on_exit;

	app_text_get_grid(&grid_cols, &rows);
	app_text_get_content_rows(&content_start_row, &content_rows);

	read_today();
	day_offset = 0;
	refresh_day();

	app_buttons_set_press_callback(on_button);
	app_buttons_set_hold_callback(on_hold);
	calendar_active = true;
}

bool app_calendar_day_is_active(void)
{
	return calendar_active;
}
