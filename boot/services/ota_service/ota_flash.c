/*****************************************************************************
 * @file    ota_flash.c
 * @brief   OTA application image Flash access implementation
 *****************************************************************************/

#include "ota_flash.h"

#include "app_mem_map.h"
#include "board_config.h"
#include "dev_flash.h"
#include "utils_lib.h"

#include <stddef.h>
#include <string.h>

#define OTA_FLASH_VERIFY_BUFFER_SIZE    64U
#define OTA_FLASH_PADDING_SIZE          2U

static uint8_t s_ota_flash_verify_buf[OTA_FLASH_VERIFY_BUFFER_SIZE];

static uint8_t ota_flash_range_is_valid(uint32_t offset, uint32_t len)
{
    if ((len == 0U) || (offset >= CONFIG_APP_SIZE))
    {
        return 0U;
    }

    return (len <= (CONFIG_APP_SIZE - offset)) ? 1U : 0U;
}

static ota_flash_result_t ota_flash_program(uint32_t addr, const uint8_t *data, uint32_t len, uint8_t erased_value)
{
    uint32_t aligned_len = len & ~1UL;
    uint8_t padding[OTA_FLASH_PADDING_SIZE];
    int ret;

    if (aligned_len > 0U)
    {
        ret = dev_flash_write(BOARD_FLASH_NAME, addr, data, aligned_len);
        if (ret != (int)aligned_len)
        {
            return OTA_FLASH_WRITE_FAILED;
        }
    }

    if (aligned_len != len)
    {
        /* The final odd byte is padded to the half-word write unit. */
        padding[0] = data[aligned_len];
        padding[1] = erased_value;
        ret = dev_flash_write(BOARD_FLASH_NAME, addr + aligned_len, padding, sizeof(padding));
        if (ret != (int)sizeof(padding))
        {
            return OTA_FLASH_WRITE_FAILED;
        }
    }

    return OTA_FLASH_OK;
}

static ota_flash_result_t ota_flash_verify(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t verified = 0U;

    while (verified < len)
    {
        uint32_t verify_len = len - verified;
        int ret;

        if (verify_len > sizeof(s_ota_flash_verify_buf))
        {
            verify_len = sizeof(s_ota_flash_verify_buf);
        }

        ret = dev_flash_read(BOARD_FLASH_NAME, addr + verified, s_ota_flash_verify_buf, verify_len);
        if ((ret != (int)verify_len) || (memcmp(s_ota_flash_verify_buf, &data[verified], verify_len) != 0))
        {
            return OTA_FLASH_READBACK_FAILED;
        }

        verified += verify_len;
    }

    return OTA_FLASH_OK;
}

ota_flash_result_t ota_flash_write_chunk(uint32_t offset, const uint8_t *data, uint32_t len)
{
    flash_info_t flash_info;
    uint32_t written = 0U;
    int ret;

    if ((data == NULL) || (ota_flash_range_is_valid(offset, len) == 0U))
    {
        return OTA_FLASH_INVALID_PARAM;
    }

    ret = dev_flash_get_info(BOARD_FLASH_NAME, &flash_info);
    if ((ret < 0) || (flash_info.page_size == 0U) ||
        (flash_info.write_unit != OTA_FLASH_PADDING_SIZE))
    {
        return OTA_FLASH_INVALID_PARAM;
    }

    if (((CONFIG_APP_BASE_ADDR + offset) % flash_info.write_unit) != 0U)
    {
        return OTA_FLASH_INVALID_PARAM;
    }

    while (written < len)
    {
        uint32_t addr = CONFIG_APP_BASE_ADDR + offset + written;
        uint32_t page_offset = addr % flash_info.page_size;
        uint32_t segment_len = len - written;
        ota_flash_result_t result;

        if (segment_len > (flash_info.page_size - page_offset))
        {
            segment_len = flash_info.page_size - page_offset;
        }

        if (page_offset == 0U)
        {
            ret = dev_flash_erase(BOARD_FLASH_NAME, addr, flash_info.page_size);
            if (ret < 0)
            {
                return OTA_FLASH_ERASE_FAILED;
            }
        }

        result = ota_flash_program(addr, &data[written], segment_len, flash_info.erased_value);
        if (result != OTA_FLASH_OK)
        {
            return result;
        }

        result = ota_flash_verify(addr, &data[written], segment_len);
        if (result != OTA_FLASH_OK)
        {
            return result;
        }

        written += segment_len;
    }

    return OTA_FLASH_OK;
}

ota_flash_result_t ota_flash_calculate_crc32(uint32_t image_size, uint32_t *crc32)
{
    uint32_t offset = 0U;
    uint32_t crc;

    if ((crc32 == NULL) || (image_size == 0U) ||
        (image_size > CONFIG_APP_SIZE))
    {
        return OTA_FLASH_INVALID_PARAM;
    }

    crc = utils_crc32_init();
    while (offset < image_size)
    {
        uint32_t read_len = image_size - offset;
        int ret;

        if (read_len > sizeof(s_ota_flash_verify_buf))
        {
            read_len = sizeof(s_ota_flash_verify_buf);
        }

        ret = dev_flash_read(BOARD_FLASH_NAME, CONFIG_APP_BASE_ADDR + offset, s_ota_flash_verify_buf, read_len);
        if (ret != (int)read_len)
        {
            return OTA_FLASH_READBACK_FAILED;
        }

        crc = utils_crc32_update(crc, s_ota_flash_verify_buf, read_len);
        offset += read_len;
    }

    *crc32 = utils_crc32_finalize(crc);
    return OTA_FLASH_OK;
}
