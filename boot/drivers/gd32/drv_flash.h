#ifndef DRV_FLASH_H
#define DRV_FLASH_H

#include <stdint.h>

typedef struct
{
    const char *name;
    uint32_t base_addr;
    uint32_t size;
    uint32_t page_size;
    uint32_t writable_base;
    uint32_t writable_size;
} gd32_flash_cfg_t;

int gd32_flash_register(const gd32_flash_cfg_t *cfg);

#endif
