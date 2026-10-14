/* SPDX-License-Identifier: Apache-2.0 */

#include "footer.h"
#include "text.h"

static uint16_t footer_row;

void app_footer_init(void)
{
	uint16_t cols;

	app_text_get_grid(&cols, &footer_row);
	footer_row -= 1;
}

int app_footer_draw(void)
{
	/* Reserved for hints; blank for now. */
	return app_text_draw_row(footer_row, "");
}
