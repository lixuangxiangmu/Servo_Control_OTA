/*****************************************************************************
 * @file    ota_flash.h
 * @brief   OTA application image Flash access interface
 *****************************************************************************/

#ifndef OTA_FLASH_H
#define OTA_FLASH_H

#include <stdint.h>

typedef enum
{
    OTA_FLASH_OK = 0,
    OTA_FLASH_INVALID_PARAM,
    OTA_FLASH_ERASE_FAILED,
    OTA_FLASH_WRITE_FAILED,
    OTA_FLASH_READBACK_FAILED,
} ota_flash_result_t;

ota_flash_result_t ota_flash_write_chunk(uint32_t offset, const uint8_t *data, uint32_t len);

ota_flash_result_t ota_flash_calculate_crc32(uint32_t image_size, uint32_t *crc32);

#endif /* OTA_FLASH_H */
