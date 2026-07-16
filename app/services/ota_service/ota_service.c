/*****************************************************************************
 * @file    ota_service.c
 * @brief   App-side OTA discovery and bootloader entry service
 *****************************************************************************/

#include "ota_service.h"

#include "app_hw.h"
#include "app_mem_map.h"
#include "dev_uart.h"
#include "ota_image.h"
#include "ota_protocol.h"
#include "return_code.h"
#include "utils_lib.h"
#include "version_config.h"

#include "FreeRTOS.h"
#include "task.h"
#include "gd32f10x.h"

#include <stddef.h>
#include <string.h>

#define APP_OTA_PRODUCT_ID              0x00001001UL
#define APP_OTA_HW_ID                   0x00000001UL
#define APP_OTA_GET_INFO_RSP_LEN        56U
#define APP_OTA_ENTER_REQ_LEN           28U
#define APP_OTA_ENTER_RSP_LEN           12U
#define APP_OTA_STATUS_RSP_LEN          4U
#define APP_OTA_REBOOT_DELAY_MS         1000U

#define APP_OTA_VERSION_NUM \
    ((((uint32_t)CONFIG_APP_VERSION_MAJOR) << 16) | \
     (((uint32_t)CONFIG_APP_VERSION_MINOR) << 8) | \
     ((uint32_t)CONFIG_APP_VERSION_PATCH))

#define APP_OTA_BOOT_VERSION_NUM \
    ((((uint32_t)CONFIG_BOOT_VERSION_MAJOR) << 16) | \
     (((uint32_t)CONFIG_BOOT_VERSION_MINOR) << 8) | \
     ((uint32_t)CONFIG_BOOT_VERSION_PATCH))

static uint8_t s_ota_tx_buf[OTA_MAX_FRAME_LEN];
static uint8_t s_ota_payload_buf[APP_OTA_GET_INFO_RSP_LEN];
static uint16_t s_ota_tx_seq;

static int ota_service_send_response(const ota_frame_t *req, const uint8_t *payload, uint16_t payload_len)
{
    ota_frame_t rsp;
    int frame_len;

    rsp.type = (uint8_t)FRAME_TYPE_RSP;
    rsp.flags = 0U;
    rsp.seq = s_ota_tx_seq++;
    rsp.ack_seq = req->seq;
    rsp.cmd = req->cmd;
    rsp.payload_len = payload_len;
    rsp.session_id = req->session_id;
    rsp.payload = payload;

    frame_len = ota_protocol_build_frame(&rsp, s_ota_tx_buf, sizeof(s_ota_tx_buf));
    if (frame_len < 0)
    {
        return frame_len;
    }

    return app_hw_bluetooth_write(s_ota_tx_buf, (uint32_t)frame_len, DEV_UART_DEFAULT_TIMEOUT_TICKS);
}

static int ota_service_send_status(const ota_frame_t *req, uint32_t status)
{
    uint8_t payload[APP_OTA_STATUS_RSP_LEN];

    utils_put_u32_le(payload, status);
    return ota_service_send_response(req, payload, sizeof(payload));
}

static void ota_service_build_info_payload(uint8_t *payload)
{
    static const uint8_t device_sn[16] =
    {
        'G', 'D', '3', '2', 'F', '1', '0', '3', 'R', 'C', 'T', '6',
        0U, 0U, 0U, 0U
    };

    memset(payload, 0, APP_OTA_GET_INFO_RSP_LEN);
    utils_put_u32_le(&payload[0], (uint32_t)OTA_OK);
    utils_put_u32_le(&payload[4], APP_OTA_PRODUCT_ID);
    utils_put_u32_le(&payload[8], APP_OTA_HW_ID);
    utils_put_u32_le(&payload[12], APP_OTA_BOOT_VERSION_NUM);
    utils_put_u32_le(&payload[16], APP_OTA_VERSION_NUM);
    utils_put_u32_le(&payload[20], CONFIG_APP_BASE_ADDR);
    utils_put_u32_le(&payload[24], CONFIG_APP_SIZE);
    utils_put_u32_le(&payload[28], FLASH_PAGE_SIZE);
    utils_put_u32_le(&payload[32], (uint32_t)OTA_PROTO_VER);
    utils_put_u32_le(&payload[36], OTA_SUPPORT_RESUME);
    memcpy(&payload[40], device_sn, sizeof(device_sn));
}

static uint32_t ota_service_prepare_enter(const ota_frame_t *req, ota_eeprom_info_t *info, uint32_t *transfer_id)
{
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t target_fw_version;
    uint32_t image_size;
    uint32_t image_crc32;

    if (req->payload_len != APP_OTA_ENTER_REQ_LEN)
    {
        return (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }

    product_id = utils_get_u32_le(&req->payload[0]);
    hw_id = utils_get_u32_le(&req->payload[4]);
    target_fw_version = utils_get_u32_le(&req->payload[8]);
    image_size = utils_get_u32_le(&req->payload[12]);
    image_crc32 = utils_get_u32_le(&req->payload[16]);
    *transfer_id = utils_get_u32_le(&req->payload[20]);

    if (product_id != APP_OTA_PRODUCT_ID)
    {
        return (uint32_t)OTA_ERR_PRODUCT_MISMATCH;
    }
    if (hw_id != APP_OTA_HW_ID)
    {
        return (uint32_t)OTA_ERR_HW_MISMATCH;
    }
    if ((target_fw_version == 0U) ||
        (target_fw_version <= APP_OTA_VERSION_NUM))
    {
        return (uint32_t)OTA_ERR_VERSION_TOO_OLD;
    }
    if ((image_size < 8U) || (image_size > CONFIG_APP_SIZE))
    {
        return (uint32_t)OTA_ERR_IMAGE_TOO_LARGE;
    }
    if ((*transfer_id == 0U) || (*transfer_id != req->session_id))
    {
        return (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }

    if (RET_IS_ERR(ota_eeprom_load(info)))
    {
        ota_eeprom_init_default(info);
    }

    info->ota_state = (uint32_t)OTA_STATE_REQUEST;
    info->image_size = image_size;
    info->image_crc32 = image_crc32;
    info->received_offset = 0U;
    info->transfer_id = *transfer_id;
    info->target_fw_version = target_fw_version;
    info->fail_reason = (uint32_t)OTA_OK;
    info->retry_count = 0U;

    if (RET_IS_ERR(ota_eeprom_save(info)))
    {
        return (uint32_t)OTA_ERR_EEPROM;
    }

    return (uint32_t)OTA_OK;
}

static int ota_service_handle_enter(const ota_frame_t *req)
{
    ota_eeprom_info_t info;
    uint32_t transfer_id = 0U;
    uint32_t status;
    int ret;

    status = ota_service_prepare_enter(req, &info, &transfer_id);
    utils_put_u32_le(&s_ota_payload_buf[0], status);
    utils_put_u32_le(&s_ota_payload_buf[4], transfer_id);
    utils_put_u32_le(&s_ota_payload_buf[8], (status == (uint32_t)OTA_OK) ? APP_OTA_REBOOT_DELAY_MS : 0U);

    ret = ota_service_send_response(req, s_ota_payload_buf, APP_OTA_ENTER_RSP_LEN);
    if ((status == (uint32_t)OTA_OK) && RET_IS_OK(ret))
    {
        /* Reset only after the response has been submitted successfully. */
        vTaskDelay(pdMS_TO_TICKS(APP_OTA_REBOOT_DELAY_MS));
        NVIC_SystemReset();
    }

    return RET_IS_ERR(ret) ? ret : ((status == (uint32_t)OTA_OK) ? RET_OK : RET_FAIL);
}

uint8_t ota_service_is_protocol_frame(const uint8_t *buf, uint32_t len)
{
    if ((buf == NULL) || (len < 2U))
    {
        return 0U;
    }

    return ((buf[0] == OTA_SOF0) && (buf[1] == OTA_SOF1)) ? 1U : 0U;
}

int ota_service_process_frame(const uint8_t *buf, uint32_t len)
{
    ota_frame_t req;
    int ret;

    ret = ota_protocol_parse_frame(buf, len, &req);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (req.type != (uint8_t)FRAME_TYPE_REQ)
    {
        return RET_INVALID_PARAM;
    }

    switch (req.cmd)
    {
        case CMD_GET_INFO:
        {
            if (req.payload_len != 0U)
            {
                return ota_service_send_status(&req, (uint32_t)OTA_ERR_PAYLOAD_LEN);
            }
            ota_service_build_info_payload(s_ota_payload_buf);
            return ota_service_send_response(&req, s_ota_payload_buf, APP_OTA_GET_INFO_RSP_LEN);
        }
        case CMD_ENTER_OTA:
        {
            return ota_service_handle_enter(&req);
        }
        default:
        {
            return ota_service_send_status(&req, (uint32_t)OTA_ERR_UNKNOWN_CMD);
        }
    }
}

void app_ota_enter(void)
{
    ota_eeprom_info_t info;

    if (RET_IS_ERR(ota_eeprom_load(&info)))
    {
        ota_eeprom_init_default(&info);
    }

    info.ota_state = (uint32_t)OTA_STATE_REQUEST;
    info.received_offset = 0U;
    info.fail_reason = (uint32_t)OTA_OK;
    info.retry_count = 0U;

    if (RET_IS_OK(ota_eeprom_save(&info)))
    {
        vTaskDelay(pdMS_TO_TICKS(200U));
        NVIC_SystemReset();
    }
}
