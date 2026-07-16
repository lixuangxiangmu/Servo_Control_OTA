#include "dev_flash.h"

#include "device.h"
#include "return_code.h"

/*****************************************************************************
@brief: Erase a specified address range in Flash
@para:name Flash device name
@para:addr Start address
@para:size Erase size in bytes
@return: RET_OK indicates erase success, other return values indicate failure
*******************************************************************************/
int dev_flash_erase(const char *name, uint32_t addr, uint32_t size)
{
    device_t *dev;
    flash_erase_config_t cfg;
    int ret;

    if ((name == 0) || (size == 0U))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    cfg.addr = addr;
    cfg.size = size;
    ret = device_control(dev, DEV_FLASH_CMD_ERASE, &cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: Read data from Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data receive buffer
@para:len Read length in bytes
@return: A positive value indicates the actual number of bytes read,
         other return values indicate failure
*******************************************************************************/
int dev_flash_read(const char *name, uint32_t addr, uint8_t *buf, uint32_t len)
{
    device_t *dev;
    flash_read_config_t cfg;

    if ((name == 0) || (buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    cfg.addr = addr;
    cfg.buf = buf;
    cfg.len = len;

    return device_control(dev, DEV_FLASH_CMD_READ, &cfg);
}

/*****************************************************************************
@brief: Write data to Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data buffer to be written
@para:len Write length in bytes
@return: A positive value indicates the actual number of bytes written,
         other return values indicate failure
*******************************************************************************/
int dev_flash_write(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    device_t *dev;
    flash_write_config_t cfg;

    if ((name == 0) || (buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    cfg.addr = addr;
    cfg.buf = buf;
    cfg.len = len;

    return device_control(dev, DEV_FLASH_CMD_WRITE, &cfg);
}

/*****************************************************************************
@brief: Get Flash geometry and writable range information
@para:name Flash device name
@para:info Output parameter used to store Flash information
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int dev_flash_get_info(const char *name, flash_info_t *info)
{
    device_t *dev;
    int ret;

    if ((name == 0) || (info == 0))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_FLASH_CMD_GET_INFO, info);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}
