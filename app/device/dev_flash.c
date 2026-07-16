#include "dev_flash.h"

#include "return_code.h"

/*****************************************************************************
@brief: Erase a specified address range in Flash
@para:name Flash device name
@para:addr Start address
@para:size Erase size in bytes
@return: Currently returns RET_NOT_SUPPORTED, indicating the Flash adapter
         driver has not yet been connected
*******************************************************************************/
int dev_flash_erase(const char *name, uint32_t addr, uint32_t size)
{
    (void)name;
    (void)addr;
    (void)size;
    return RET_NOT_SUPPORTED;
}

/*****************************************************************************
@brief: Read data from Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data receive buffer
@para:len Read length in bytes
@return: Currently returns RET_NOT_SUPPORTED, indicating the Flash adapter
         driver has not yet been connected
*******************************************************************************/
int dev_flash_read(const char *name, uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void)name;
    (void)addr;
    (void)buf;
    (void)len;
    return RET_NOT_SUPPORTED;
}

/*****************************************************************************
@brief: Write data to Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data buffer to be written
@para:len Write length in bytes
@return: Currently returns RET_NOT_SUPPORTED, indicating the Flash adapter
         driver has not yet been connected
*******************************************************************************/
int dev_flash_write(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    (void)name;
    (void)addr;
    (void)buf;
    (void)len;
    return RET_NOT_SUPPORTED;
}
