/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>

#include "font_tamzen.h"
#include "footer.h"
#include "fuel_gauge.h"
#include "max17055.h"
#include "text.h"

static uint16_t footer_row;
static int32_t last_instant_current_ua;

void app_footer_init(void)
{
	uint16_t cols;

	app_text_get_grid(&cols, &footer_row);
	footer_row -= 1;
}

void app_footer_sample_current(void)
{
	int32_t current_ua;

	if (app_fuel_gauge_read_current_ua(&current_ua) == 0) {
		last_instant_current_ua = current_ua;
	}
}

static void format_current_ma(char *out, size_t len, int32_t current_ua)
{
	bool neg = current_ua < 0;

	if (neg) {
		current_ua = -current_ua;
	}

	snprintf(out, len, "%s%d.%03dmA", neg ? "-" : "", (int)(current_ua / 1000),
		 (int)(current_ua % 1000));
}

int app_footer_draw(void)
{
	struct max17055_data_snapshot snap;
	char buf[72];
	char inst_buf[16];
	char avg_buf[16];
	int ret = app_fuel_gauge_read(&snap);

	if (ret) {
		return ret;
	}

	format_current_ma(inst_buf, sizeof(inst_buf), last_instant_current_ua);
	format_current_ma(avg_buf, sizeof(avg_buf), snap.avg_current_ua);

	snprintf(buf, sizeof(buf), "%u.%u%%  %d.%03dV  I%s A%s", snap.soc_x10 / 10,
		 snap.soc_x10 % 10, (int)(snap.voltage_uv / 1000000),
		 (int)((snap.voltage_uv / 1000) % 1000), inst_buf, avg_buf);

	app_text_draw_row_bottom_aligned(footer_row, buf, FONT_TAMZEN_NATIVE_HEIGHT);

	return 0;
}
