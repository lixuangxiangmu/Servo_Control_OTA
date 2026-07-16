#ifndef DEV_FLASH_H
#define DEV_FLASH_H

#include <stdint.h>

/*****************************************************************************
@brief: Erase a specified address range in Flash
@para:name Flash device name
@para:addr Start address
@para:size Erase size in bytes
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

#endif
