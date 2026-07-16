#ifndef APP_DISPLAY_H
#define APP_DISPLAY_H

#include <stdint.h>

/*===========================================================================
 * Public Interface
 *===========================================================================*/

int app_display_init(void);
int app_display_cycle_edit_mode(void);
int app_display_adjust(int8_t step);
int app_display_exit_edit(uint8_t save);

#endif /* APP_DISPLAY_H */
