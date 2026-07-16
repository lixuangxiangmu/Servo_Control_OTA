#ifndef DEV_FLASH_H
#define DEV_FLASH_H

#include <stdint.h>

/* Flash device control commands. Upper layers should prefer dev_flash_xxx()
   instead of calling device_control() directly. */
#define DEV_FLASH_CMD_ERASE       1
#define DEV_FLASH_CMD_READ        2
#define DEV_FLASH_CMD_WRITE       3
#define DEV_FLASH_CMD_GET_INFO    4

typedef struct
{
    uint32_t base_addr;
    uint32_t size;
    uint32_t page_size;
    uint32_t write_unit;
    uint32_t writable_base;
    uint32_t writable_size;
    uint8_t erased_value;
} flash_info_t;

typedef struct
{
    uint32_t addr;
    uint32_t size;
} flash_erase_config_t;

typedef struct
{
    uint32_t addr;
    uint8_t *buf;
    uint32_t len;
} flash_read_config_t;

typedef struct
{
    uint32_t addr;
    const uint8_t *buf;
    uint32_t len;
} flash_write_config_t;

/*****************************************************************************
@brief: Erase a specified address range in Flash
@para:name Flash device name
@para:addr Start address
@para:size Erase size in bytes. The driver erases all pages covered by this
           range, and addr must be aligned to the Flash page size.
@return: RET_OK indicates erase success, other return values indicate failure
*******************************************************************************/
int dev_flash_erase(const char *name, uint32_t addr, uint32_t size);

/*****************************************************************************
@brief: Read data from Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data receive buffer
@para:len Read length in bytes
@return: A positive value indicates the actual number of bytes read,
         other return values indicate failure
*******************************************************************************/
int dev_flash_read(const char *name, uint32_t addr, uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: Write data to Flash
@para:name Flash device name
@para:addr Start address
@para:buf Data buffer to be written
@para:len Write length in bytes
@return: A positive value indicates the actual number of bytes written,
         other return values indicate failure
*******************************************************************************/
int dev_flash_write(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: Get Flash geometry and writable range information
@para:name Flash device name
@para:info Output parameter used to store Flash information
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int dev_flash_get_info(const char *name, flash_info_t *info);

#endif
