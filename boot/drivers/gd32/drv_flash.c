#include "drv_flash.h"

#include "device.h"
#include "dev_flash.h"
#include "gd32f10x_fmc.h"
#include "return_code.h"

#include <stdint.h>

#define GD32_FLASH_DEVICE_MAX     1U
#define GD32_FLASH_WRITE_UNIT     2U
#define GD32_FLASH_ERASED_VALUE   0xFFU

typedef struct
{
    uint32_t base_addr;
    uint32_t size;
    uint32_t page_size;
    uint32_t writable_base;
    uint32_t writable_size;
} gd32_flash_dev_t;

typedef struct
{
    device_t dev;
    gd32_flash_dev_t flash;
} gd32_flash_slot_t;

static gd32_flash_slot_t s_flash_slots[GD32_FLASH_DEVICE_MAX];
static uint8_t s_flash_count;

static uint8_t gd32_flash_range_is_valid(uint32_t addr, uint32_t len, uint32_t base, uint32_t size)
{
    uint32_t last_addr;
    uint32_t end_addr;

    if ((len == 0U) || (size == 0U))
    {
        return 0U;
    }

    last_addr = addr + len - 1U;
    if (last_addr < addr)
    {
        return 0U;
    }

    end_addr = base + size - 1U;
    if (end_addr < base)
    {
        return 0U;
    }

    return ((addr >= base) && (last_addr <= end_addr)) ? 1U : 0U;
}

static int gd32_flash_get_erase_size(const gd32_flash_dev_t *flash, uint32_t size, uint32_t *erase_size)
{
    uint32_t page_count;

    if ((flash == 0) || (erase_size == 0) || (size == 0U) || (flash->page_size == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (size > (UINT32_MAX - (flash->page_size - 1U)))
    {
        return RET_INVALID_PARAM;
    }

    page_count = (size + flash->page_size - 1U) / flash->page_size;
    if (page_count > (UINT32_MAX / flash->page_size))
    {
        return RET_INVALID_PARAM;
    }

    *erase_size = page_count * flash->page_size;

    return RET_OK;
}

static int gd32_flash_state_to_ret(fmc_state_enum state)
{
    switch (state)
    {
    case FMC_READY:
        return RET_OK;

    case FMC_BUSY:
        return RET_BUSY;

    case FMC_TOERR:
        return RET_TIMEOUT;

    case FMC_PGERR:
    case FMC_WPERR:
    default:
        return RET_IO_ERROR;
    }
}

static int gd32_flash_init(device_t *dev)
{
    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    return RET_OK;
}

static int gd32_flash_read_data(const gd32_flash_dev_t *flash, uint32_t addr, uint8_t *buf, uint32_t len)
{
    volatile const uint8_t *src;
    uint32_t i;

    if ((flash == 0) || (buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (gd32_flash_range_is_valid(addr, len, flash->base_addr, flash->size) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    src = (volatile const uint8_t *)addr;
    for (i = 0U; i < len; i++)
    {
        buf[i] = src[i];
    }

    return RET_SUCCESS_VALUE(len);
}

static int gd32_flash_verify_erased(uint32_t addr, uint32_t size)
{
    volatile const uint8_t *src = (volatile const uint8_t *)addr;
    uint32_t i;

    for (i = 0U; i < size; i++)
    {
        if (src[i] != GD32_FLASH_ERASED_VALUE)
        {
            return RET_IO_ERROR;
        }
    }

    return RET_OK;
}

static int gd32_flash_erase_pages(const gd32_flash_dev_t *flash, uint32_t addr, uint32_t size)
{
    uint32_t erase_size;
    uint32_t page_addr;
    uint32_t erased;
    int ret;

    if ((flash == 0) || (size == 0U) || ((addr % flash->page_size) != 0U))
    {
        return RET_INVALID_PARAM;
    }

    ret = gd32_flash_get_erase_size(flash, size, &erase_size);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (gd32_flash_range_is_valid(addr, erase_size, flash->writable_base, flash->writable_size) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    fmc_unlock();

    erased = 0U;
    while (erased < erase_size)
    {
        page_addr = addr + erased;
        ret = gd32_flash_state_to_ret(fmc_page_erase(page_addr));
        if (RET_IS_ERR(ret))
        {
            fmc_lock();
            return ret;
        }

        erased += flash->page_size;
    }

    fmc_lock();

    return gd32_flash_verify_erased(addr, erase_size);
}

static int gd32_flash_write_data(const gd32_flash_dev_t *flash, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t offset;
    uint16_t halfword;
    int ret;

    if ((flash == 0) || (buf == 0) || (len == 0U) ||
        ((addr % GD32_FLASH_WRITE_UNIT) != 0U) || ((len % GD32_FLASH_WRITE_UNIT) != 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (gd32_flash_range_is_valid(addr, len, flash->writable_base, flash->writable_size) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    fmc_unlock();

    for (offset = 0U; offset < len; offset += GD32_FLASH_WRITE_UNIT)
    {
        halfword = (uint16_t)buf[offset] | ((uint16_t)buf[offset + 1U] << 8);
        ret = gd32_flash_state_to_ret(fmc_halfword_program(addr + offset, halfword));
        if (RET_IS_ERR(ret))
        {
            fmc_lock();
            return ret;
        }

        if (*(volatile const uint16_t *)(addr + offset) != halfword)
        {
            fmc_lock();
            return RET_IO_ERROR;
        }
    }

    fmc_lock();

    return RET_SUCCESS_VALUE(len);
}

static int gd32_flash_get_info(const gd32_flash_dev_t *flash, flash_info_t *info)
{
    if ((flash == 0) || (info == 0))
    {
        return RET_INVALID_PARAM;
    }

    info->base_addr = flash->base_addr;
    info->size = flash->size;
    info->page_size = flash->page_size;
    info->write_unit = GD32_FLASH_WRITE_UNIT;
    info->writable_base = flash->writable_base;
    info->writable_size = flash->writable_size;
    info->erased_value = GD32_FLASH_ERASED_VALUE;

    return RET_OK;
}

static int gd32_flash_control(device_t *dev, int cmd, void *arg)
{
    gd32_flash_dev_t *flash;
    const flash_erase_config_t *erase_cfg;
    const flash_read_config_t *read_cfg;
    const flash_write_config_t *write_cfg;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    flash = (gd32_flash_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_FLASH_CMD_ERASE:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        erase_cfg = (const flash_erase_config_t *)arg;
        return gd32_flash_erase_pages(flash, erase_cfg->addr, erase_cfg->size);

    case DEV_FLASH_CMD_READ:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        read_cfg = (const flash_read_config_t *)arg;
        return gd32_flash_read_data(flash, read_cfg->addr, read_cfg->buf, read_cfg->len);

    case DEV_FLASH_CMD_WRITE:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        write_cfg = (const flash_write_config_t *)arg;
        return gd32_flash_write_data(flash, write_cfg->addr, write_cfg->buf, write_cfg->len);

    case DEV_FLASH_CMD_GET_INFO:
        return gd32_flash_get_info(flash, (flash_info_t *)arg);

    default:
        return RET_NOT_SUPPORTED;
    }
}

static const device_ops_t s_gd32_flash_ops =
{
    gd32_flash_init,
    0,
    0,
    0,
    0,
    gd32_flash_control,
};

int gd32_flash_register(const gd32_flash_cfg_t *cfg)
{
    gd32_flash_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->page_size == 0U) ||
        (cfg->size == 0U) || (cfg->writable_size == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (((cfg->base_addr % cfg->page_size) != 0U) ||
        ((cfg->writable_base % cfg->page_size) != 0U) ||
        ((cfg->writable_size % cfg->page_size) != 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (gd32_flash_range_is_valid(cfg->writable_base,
                                  cfg->writable_size,
                                  cfg->base_addr,
                                  cfg->size) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    if (device_find(cfg->name) != 0)
    {
        return RET_ALREADY_EXISTS;
    }

    if (s_flash_count >= GD32_FLASH_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    slot = &s_flash_slots[s_flash_count];
    slot->flash.base_addr = cfg->base_addr;
    slot->flash.size = cfg->size;
    slot->flash.page_size = cfg->page_size;
    slot->flash.writable_base = cfg->writable_base;
    slot->flash.writable_size = cfg->writable_size;

    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_FLASH;
    slot->dev.ops = &s_gd32_flash_ops;
    slot->dev.user_data = &slot->flash;
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_flash_count++;
    }

    return ret;
}
