#ifndef DRV_GPIO_H
#define DRV_GPIO_H

#include "gd32f10x_libopt.h"

#include <stdint.h>

typedef struct
{
    const char *name;
    rcu_periph_enum rcu;
    uint32_t gpio_periph;
    uint32_t pin;
    uint32_t mode;
    uint32_t speed;
    uint8_t default_level;
} gd32_gpio_cfg_t;

int gd32_gpio_register(const gd32_gpio_cfg_t *cfg);

#endif
