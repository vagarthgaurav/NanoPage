/* SPDX-License-Identifier: Apache-2.0 */

#ifndef APP_SLEEP_SCREEN_H_
#define APP_SLEEP_SCREEN_H_

/* Draws the small NanoPage logo in the top row, today's date centred below it,
 * then the calendar day view, then does a full refresh. Called right before System OFF, it takes
 * over the text framebuffer and switches to the Tamzen font.
 * Returns 0 on success, negative errno on failure.
 */
int app_sleep_screen_show(void);

#endif /* APP_SLEEP_SCREEN_H_ */
