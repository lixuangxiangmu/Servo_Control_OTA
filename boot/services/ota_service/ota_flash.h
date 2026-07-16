/*****************************************************************************
 * @file    ota_flash.h
 * @brief   OTA application image Flash access interface
 *
 * This module exposes the bootloader Flash operations required by the OTA
 * service. All offsets are relative to CONFIG_APP_BASE_ADDR and are checked
 * against CONFIG_APP_SIZE before the lower Flash driver is called.
 *****************************************************************************/

#ifndef OTA_FLASH_H
#define OTA_FLASH_H

#include <stdint.h>

/**
 * @brief Result codes returned by OTA Flash operations.
 */
typedef enum
{
    OTA_FLASH_OK = 0,          /**< Operation completed successfully. */
    OTA_FLASH_INVALID_PARAM,   /**< Invalid pointer, size, offset, or Flash geometry. */
    OTA_FLASH_ERASE_FAILED,    /**< Flash page erase failed. */
    OTA_FLASH_WRITE_FAILED,    /**< Flash program operation failed. */
    OTA_FLASH_READBACK_FAILED, /**< Flash read or read-back comparison failed. */
} ota_flash_result_t;

/*****************************************************************************
 * @brief Write one OTA data chunk into the application Flash area.
 *
 * The function validates the target range, erases each page when the chunk
 * starts at that page boundary, writes the data, then reads the same bytes back
 * for verification. Odd final image bytes are padded internally to satisfy the
 * Flash half-word write unit.
 *
 * @param offset Offset from CONFIG_APP_BASE_ADDR.
 * @param data   Data buffer to be programmed.
 * @param len    Number of bytes to write.
 *
 * @return OTA_FLASH_OK on success, otherwise an ota_flash_result_t error code.
 *****************************************************************************/
ota_flash_result_t ota_flash_write_chunk(uint32_t offset, const uint8_t *data, uint32_t len);

/*****************************************************************************
 * @brief Calculate CRC32 over the programmed application image.
 *
 * Reads the application Flash area from CONFIG_APP_BASE_ADDR in small blocks and
 * updates the CRC32 context until @p image_size bytes have been processed.
 *
 * @param image_size Number of image bytes to include in the CRC.
 * @param crc32      Output pointer for the finalized CRC32 value.
 *
 * @return OTA_FLASH_OK on success, otherwise an ota_flash_result_t error code.
 *****************************************************************************/
ota_flash_result_t ota_flash_calculate_crc32(uint32_t image_size, uint32_t *crc32);

#endif /* OTA_FLASH_H */
