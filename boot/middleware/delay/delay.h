#ifndef __DELAY_H__
#define __DELAY_H__

#include <stdint.h>

void delay_init(void);
void delay_us(uint32_t nUs);
void delay_ms(uint32_t nMs);

#endif