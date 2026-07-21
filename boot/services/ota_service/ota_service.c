/*****************************************************************************
 * @file    ota_service.c
 * @brief   Bootloader OTA protocol, Flash update, and recovery service
 *****************************************************************************/

#include "ota_service.h"

#include "../../application/app_config.h"
#include "app_main.h"
#include "app_mem_map.h"
#include "board_config.h"
#include "delay.h"
#include "../../device/dev_uart.h"
#include "ota_flash.h"
#include "ota_protocol.h"
#include "return_code.h"
#include "utils_lib.h"
#include "version_config.h"

#include <stdint.h>
#include <string.h>

#define OTA_BOOT_VERSION_NUM \
    ((((uint32_t)CONFIG_BOOT_VERSION_MAJOR) << 16) | \
     (((uint32_t)CONFIG_BOOT_VERSION_MINOR) << 8) | \
     ((uint32_t)CONFIG_BOOT_VERSION_PATCH))

#define OTA_PING_RSP_LEN              16U
#define OTA_STATUS_ONLY_RSP_LEN       4U
#define OTA_GET_INFO_RSP_LEN          56U
#define OTA_GET_STATUS_RSP_LEN        32U
#define OTA_START_REQ_LEN             32U
#define OTA_START_RSP_LEN             20U
#define OTA_DATA_REQ_HEADER_LEN       8U
#define OTA_DATA_RSP_LEN              12U
#define OTA_END_REQ_LEN               12U
#define OTA_END_RSP_LEN               12U
#define OTA_ERASE_POLICY_ON_DEMAND    1U
#define OTA_END_REBOOT_DELAY_MS       500U
#define OTA_MAX_CHUNK_SIZE            (OTA_MAX_PAYLOAD - OTA_DATA_REQ_HEADER_LEN)

/* Temporary diagnostics: set to 0U after the CRC issue is resolved. */
#define OTA_CRC_DEBUG_EVENT_ENABLED   0U
#define OTA_CRC_DEBUG_EVENT_CMD       ((uint16_t)(CMD_VENDOR_BEGIN + 1U))
#define OTA_CRC_DEBUG_EVENT_LEN       20U
#define OTA_CRC_DEBUG_MAGIC           0x31474244UL /* "DBG1", little-endian */
#define OTA_CRC_DEBUG_TX_RETRY_COUNT  20U

static uint8_t s_ota_rx_buf[DEV_UART_FRAME_MAX_LEN];
static uint8_t s_ota_tx_buf[DEV_UART_FRAME_MAX_LEN];
static uint8_t s_ota_payload_buf[OTA_GET_INFO_RSP_LEN];
static uint16_t s_ota_tx_seq;
static uint32_t s_ota_chunk_size;
static uint8_t s_ota_transfer_active;

typedef struct
{
    uint32_t session_id;
    uint32_t image_size;
} ota_crc_debug_context_t;

/**
 * @brief Load persistent OTA state and determine whether the application is valid.
 *
 * Initializes default state when the EEPROM record cannot be loaded. An image is
 * considered invalid while a download or verification is in progress, or after
 * a terminal failure; otherwise, its validity is read from the application.
 *
 * @param info Destination for the OTA runtime information.
 */
static void ota_service_load_runtime_info(ota_eeprom_info_t *info)
{
    if (RET_IS_ERR(ota_eeprom_load(info)))
    {
        ota_eeprom_init_default(info);
    }

    if ((info->ota_state == (uint32_t)OTA_STATE_DOWNLOADING) ||
        (info->ota_state == (uint32_t)OTA_STATE_VERIFYING) ||
        (info->ota_state == (uint32_t)OTA_STATE_FAILED))
    {
        info->app_valid = 0U;
    }
    else
    {
        info->app_valid = (uint32_t)boot_app_is_valid();
    }
}

/**
 * @brief Build the payload returned for a PING command.
 *
 * The payload contains OTA_OK, the protocol version, the packed bootloader
 * version, and the current application-validity flag.
 *
 * @param payload Destination buffer of at least OTA_PING_RSP_LEN bytes.
 * @param info    Current OTA runtime information.
 */
static void ota_service_build_ping_payload(uint8_t *payload, const ota_eeprom_info_t *info)
{
    utils_put_u32_le(&payload[0], (uint32_t)OTA_OK);
    utils_put_u32_le(&payload[4], (uint32_t)OTA_PROTO_VER);
    utils_put_u32_le(&payload[8], OTA_BOOT_VERSION_NUM);
    utils_put_u32_le(&payload[12], info->app_valid);
}

/**
 * @brief Build the device identification and OTA capability response payload.
 *
 * Clears the complete response before encoding product, hardware, version,
 * application Flash, protocol capability, and serial-number fields.
 *
 * @param payload Destination buffer of at least OTA_GET_INFO_RSP_LEN bytes.
 * @param info    Current OTA runtime information containing the target version.
 */
static void ota_service_build_get_info_payload(uint8_t *payload, const ota_eeprom_info_t *info)
{
    static const uint8_t device_sn[16] =
    {
        'G', 'D', '3', '2', 'F', '1', '0', '3', 'R', 'C', 'T', '6',
        0U, 0U, 0U, 0U
    };

    memset(payload, 0, OTA_GET_INFO_RSP_LEN);
    utils_put_u32_le(&payload[0], (uint32_t)OTA_OK);
    utils_put_u32_le(&payload[4], BOOT_PRODUCT_ID);
    utils_put_u32_le(&payload[8], BOOT_FW_HW_ID);
    utils_put_u32_le(&payload[12], OTA_BOOT_VERSION_NUM);
    utils_put_u32_le(&payload[16], info->target_fw_version);
    utils_put_u32_le(&payload[20], CONFIG_APP_BASE_ADDR);
    utils_put_u32_le(&payload[24], CONFIG_APP_SIZE);
    utils_put_u32_le(&payload[28], FLASH_PAGE_SIZE);
    utils_put_u32_le(&payload[32], (uint32_t)OTA_PROTO_VER);
    utils_put_u32_le(&payload[36], OTA_SUPPORT_RESUME);
    memcpy(&payload[40], device_sn, sizeof(device_sn));
}

/**
 * @brief Build the payload that reports the current OTA transfer status.
 *
 * The response includes persisted transfer identity, image metadata, received
 * offset, application validity, and the most recent failure status.
 *
 * @param payload Destination buffer of at least OTA_GET_STATUS_RSP_LEN bytes.
 * @param info    Current OTA runtime information.
 */
static void ota_service_build_get_status_payload(uint8_t *payload, const ota_eeprom_info_t *info)
{
    utils_put_u32_le(&payload[0], (uint32_t)OTA_OK);
    utils_put_u32_le(&payload[4], info->ota_state);
    utils_put_u32_le(&payload[8], info->transfer_id);
    utils_put_u32_le(&payload[12], info->image_size);
    utils_put_u32_le(&payload[16], info->image_crc32);
    utils_put_u32_le(&payload[20], info->received_offset);
    utils_put_u32_le(&payload[24], info->app_valid);
    utils_put_u32_le(&payload[28], info->fail_reason);
}

/**
 * @brief Build the response payload for an OTA_START command.
 *
 * Encodes the result, transfer identifier, negotiated chunk size, resume
 * offset, and the on-demand Flash erase policy.
 *
 * @param payload     Destination buffer of at least OTA_START_RSP_LEN bytes.
 * @param status      Result status for the request.
 * @param transfer_id Transfer identifier returned to the host.
 * @param chunk_size  Negotiated data chunk size.
 * @param next_offset Offset at which the host must send the next data chunk.
 */
static void ota_service_build_start_payload(uint8_t *payload,
                                            uint32_t status,
                                            uint32_t transfer_id,
                                            uint32_t chunk_size,
                                            uint32_t next_offset)
{
    utils_put_u32_le(&payload[0], status);
    utils_put_u32_le(&payload[4], transfer_id);
    utils_put_u32_le(&payload[8], chunk_size);
    utils_put_u32_le(&payload[12], next_offset);
    utils_put_u32_le(&payload[16], OTA_ERASE_POLICY_ON_DEMAND);
}

/**
 * @brief Build the response payload that acknowledges an OTA_DATA command.
 *
 * Returns the processed offset and the sequence number expected for the next
 * request, together with the result status.
 *
 * @param payload         Destination buffer of at least OTA_DATA_RSP_LEN bytes.
 * @param status          Result status for the request.
 * @param received_offset Number of image bytes committed by the device.
 * @param expect_seq      Sequence number expected from the host next.
 */
static void ota_service_build_data_payload(uint8_t *payload,
                                           uint32_t status,
                                           uint32_t received_offset,
                                           uint16_t expect_seq)
{
    utils_put_u32_le(&payload[0], status);
    utils_put_u32_le(&payload[4], received_offset);
    utils_put_u32_le(&payload[8], (uint32_t)expect_seq);
}

/**
 * @brief Build the response payload for an OTA_END command.
 *
 * Reports the final status, the calculated full-image CRC32, and the delay
 * before reboot. The delay is non-zero only after a successful update.
 *
 * @param payload            Destination buffer of at least OTA_END_RSP_LEN bytes.
 * @param status             Result status for the request.
 * @param image_crc32_calc   CRC32 calculated from the application Flash area.
 * @param reboot_delay_ms    Delay before jumping to the updated application.
 */
static void ota_service_build_end_payload(uint8_t *payload,
                                          uint32_t status,
                                          uint32_t image_crc32_calc,
                                          uint32_t reboot_delay_ms)
{
    utils_put_u32_le(&payload[0], status);
    utils_put_u32_le(&payload[4], image_crc32_calc);
    utils_put_u32_le(&payload[8], reboot_delay_ms);
}

/**
 * @brief Build and transmit an OTA response frame for a request.
 *
 * The response copies the command and session identifier from @p req, uses a
 * monotonically incremented transmit sequence number, and acknowledges the
 * request sequence number before it is framed and sent through UART3.
 *
 * @param req         Request frame being acknowledged.
 * @param payload     Response payload, or NULL when @p payload_len is zero.
 * @param payload_len Length of @p payload in bytes.
 *
 * @return The UART write result, or the frame-builder result when framing fails.
 */
static int ota_service_send_response(const ota_frame_t *req, const uint8_t *payload, uint16_t payload_len)
{
    ota_frame_t rsp;
    int tx_len;

    rsp.type = (uint8_t)FRAME_TYPE_RSP;
    rsp.flags = 0U;
    rsp.seq = s_ota_tx_seq++;
    rsp.ack_seq = req->seq;
    rsp.cmd = req->cmd;
    rsp.payload_len = payload_len;
    rsp.session_id = req->session_id;
    rsp.payload = payload;

    tx_len = ota_protocol_build_frame(&rsp, s_ota_tx_buf, sizeof(s_ota_tx_buf));
    if (tx_len > 0)
    {
        return dev_uart_write(BOARD_UART3_NAME, s_ota_tx_buf, (uint32_t)tx_len);
    }

    return tx_len;
}

/**
 * @brief Send one temporary CRC-progress EVENT frame to the OTA host.
 *
 * Payload layout (all uint32 little-endian):
 *   0: 0x31474244 ("DBG1")
 *   4: processed image bytes
 *   8: finalized CRC32 of bytes [0, processed)
 *  12: total image bytes
 *  16: APP Flash base address
 */
static void ota_service_send_crc_debug_event(uint32_t processed_bytes,
                                             uint32_t crc32_so_far,
                                             void *user_ctx)
{
#if (OTA_CRC_DEBUG_EVENT_ENABLED != 0U)
    const ota_crc_debug_context_t *ctx = (const ota_crc_debug_context_t *)user_ctx;
    ota_frame_t event;
    uint8_t payload[OTA_CRC_DEBUG_EVENT_LEN];
    int frame_len;
    int tx_ret;
    uint8_t retry;

    if (ctx == NULL)
    {
        return;
    }

    utils_put_u32_le(&payload[0], OTA_CRC_DEBUG_MAGIC);
    utils_put_u32_le(&payload[4], processed_bytes);
    utils_put_u32_le(&payload[8], crc32_so_far);
    utils_put_u32_le(&payload[12], ctx->image_size);
    utils_put_u32_le(&payload[16], CONFIG_APP_BASE_ADDR);

    event.type = (uint8_t)FRAME_TYPE_EVENT;
    event.flags = 0U;
    event.seq = s_ota_tx_seq++;
    event.ack_seq = 0U;
    event.cmd = OTA_CRC_DEBUG_EVENT_CMD;
    event.payload_len = sizeof(payload);
    event.session_id = ctx->session_id;
    event.payload = payload;

    frame_len = ota_protocol_build_frame(&event, s_ota_tx_buf, sizeof(s_ota_tx_buf));
    if (frame_len > 0)
    {
        /* Wait briefly for the preceding UART-DMA transfer instead of dropping a page event. */
        for (retry = 0U; retry < OTA_CRC_DEBUG_TX_RETRY_COUNT; retry++)
        {
            tx_ret = dev_uart_write(BOARD_UART3_NAME, s_ota_tx_buf, (uint32_t)frame_len);
            if (tx_ret > 0)
            {
                /* A 42-byte event takes < 4 ms at 115200 baud; retain margin for DMA completion. */
                delay_ms(10U);
                break;
            }

            delay_ms(1U);
        }
    }
#else
    (void)processed_bytes;
    (void)crc32_so_far;
    (void)user_ctx;
#endif
}

/**
 * @brief Send a response containing only an OTA status code.
 *
 * @param req    Request frame being acknowledged.
 * @param status Result status to encode in the response payload.
 */
static void ota_service_send_status_response(const ota_frame_t *req, uint32_t status)
{
    uint8_t payload[OTA_STATUS_ONLY_RSP_LEN];

    utils_put_u32_le(payload, status);
    ota_service_send_response(req, payload, sizeof(payload));
}

/**
 * @brief Normalize a requested data chunk size to a supported Flash-write size.
 *
 * A zero request selects the default block size. Oversized requests are capped
 * to the protocol payload limit, and the result is rounded down to an even
 * half-word boundary required by normal Flash writes.
 *
 * @param requested_size Chunk size requested by the host.
 *
 * @return A supported chunk size, or zero if it is below BOOT_MIN_BLOCK_SIZE.
 */
static uint32_t ota_service_negotiate_chunk_size(uint32_t requested_size)
{
    uint32_t chunk_size = requested_size;

    if (chunk_size == 0U)
    {
        chunk_size = BOOT_DEFAULT_BLOCK_SIZE;
    }
    if (chunk_size > OTA_MAX_CHUNK_SIZE)
    {
        chunk_size = OTA_MAX_CHUNK_SIZE;
    }

    /* Normal Flash writes end at an even half-word boundary. */
    chunk_size &= ~1UL;
    return (chunk_size >= BOOT_MIN_BLOCK_SIZE) ? chunk_size : 0U;
}

/**
 * @brief Determine whether an OTA_START request can resume the active download.
 *
 * A request is resumable only when the persisted state is DOWNLOADING and all
 * transfer identity and image metadata fields match the saved transfer.
 *
 * @param info        Persisted OTA runtime information.
 * @param transfer_id Requested transfer identifier.
 * @param fw_version  Requested target firmware version.
 * @param image_size  Requested image size in bytes.
 * @param image_crc32 Requested full-image CRC32.
 *
 * @return 1 if the transfer can resume; otherwise 0.
 */
static uint8_t ota_service_start_is_resume( const ota_eeprom_info_t *info,
                                            uint32_t transfer_id,
                                            uint32_t fw_version,
                                            uint32_t image_size,
                                            uint32_t image_crc32)
{
    if (info->ota_state != (uint32_t)OTA_STATE_DOWNLOADING)
    {
        return 0U;
    }

    return ((info->transfer_id == transfer_id) &&
            (info->target_fw_version == fw_version) &&
            (info->image_size == image_size) &&
            (info->image_crc32 == image_crc32) &&
            (info->received_offset <= image_size)) ? 1U : 0U;
}

/**
 * @brief Persist a terminal OTA failure and invalidate the application image.
 *
 * Clears both application validity and confirmation flags before saving the
 * failure state. The EEPROM save result is deliberately ignored.
 *
 * @param info   OTA runtime information to update.
 * @param status Terminal failure status to record.
 */
static void ota_service_record_failure(ota_eeprom_info_t *info, uint32_t status)
{
    info->ota_state = (uint32_t)OTA_STATE_FAILED;
    info->app_valid = 0U;
    info->app_confirmed = 0U;
    info->fail_reason = status;
    (void)ota_eeprom_save(info);
}

/**
 * @brief Record a non-terminal transfer error and increment its retry counter.
 *
 * The retry counter saturates at UINT32_MAX. This helper updates RAM state
 * only; the state is persisted later at a checkpoint or terminal transition.
 *
 * @param info   OTA runtime information to update.
 * @param status Recoverable failure status to record.
 */
static void ota_service_record_recoverable_error(ota_eeprom_info_t *info, uint32_t status)
{
    if (info->retry_count < UINT32_MAX)
    {
        info->retry_count++;
    }
    info->fail_reason = status;
}

/**
 * @brief Save a download-progress checkpoint after crossing a Flash page boundary.
 *
 * Persists only completely received Flash pages, so a resumed download starts
 * at the beginning of the last incomplete page.
 *
 * @param info            OTA runtime information after the data write.
 * @param previous_offset Received offset before the data write.
 *
 * @return RET_OK if no new complete page exists or saving succeeds; otherwise
 *         the EEPROM save error code.
 */
static int ota_service_save_page_checkpoint(const ota_eeprom_info_t *info, uint32_t previous_offset)
{
    uint32_t previous_page_end;
    uint32_t current_page_end;
    ota_eeprom_info_t checkpoint;

    previous_page_end = previous_offset - (previous_offset % FLASH_PAGE_SIZE);
    current_page_end = info->received_offset - (info->received_offset % FLASH_PAGE_SIZE);
    if (current_page_end <= previous_page_end)
    {
        return RET_OK;
    }

    checkpoint = *info;
    checkpoint.received_offset = current_page_end;
    return ota_eeprom_save(&checkpoint);
}

/**
 * @brief Convert an OTA Flash-layer result into its protocol status code.
 *
 * @param result Result returned by an OTA Flash operation.
 *
 * @return Corresponding OTA error status. Invalid parameters map to
 *         OTA_ERR_PAYLOAD_LEN.
 */
static uint32_t ota_service_flash_result_to_status(ota_flash_result_t result)
{
    switch (result)
    {
        case OTA_FLASH_ERASE_FAILED:
        {
            return (uint32_t)OTA_ERR_FLASH_ERASE;
        }
        case OTA_FLASH_WRITE_FAILED:
        {
            return (uint32_t)OTA_ERR_FLASH_WRITE;
        }
        case OTA_FLASH_READBACK_FAILED:
        {
            return (uint32_t)OTA_ERR_FLASH_READBACK;
        }
        case OTA_FLASH_INVALID_PARAM:
        default:
        {
            return (uint32_t)OTA_ERR_PAYLOAD_LEN;
        }
    }
}

/**
 * @brief Validate and initialize, or resume, an OTA transfer requested by OTA_START.
 *
 * Validates compatibility, version, image bounds, session identity, and chunk
 * size. A matching in-progress transfer resumes at its saved offset; otherwise
 * a new DOWNLOADING state is saved before enabling data transfer.
 *
 * @param req  Parsed OTA_START request frame.
 * @param info Current OTA runtime information to inspect and update.
 */
static void ota_service_handle_start(const ota_frame_t *req, ota_eeprom_info_t *info)
{
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t fw_version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t transfer_id;
    uint32_t chunk_size;
    uint32_t status = (uint32_t)OTA_OK;
    uint32_t next_offset = 0U;
    ota_eeprom_info_t new_info;

    if (req->payload_len != OTA_START_REQ_LEN)
    {
        ota_service_build_start_payload(s_ota_payload_buf, (uint32_t)OTA_ERR_PAYLOAD_LEN, 0U, 0U, 0U);
        ota_service_send_response(req, s_ota_payload_buf, OTA_START_RSP_LEN);
        return;
    }

    product_id = utils_get_u32_le(&req->payload[0]);
    hw_id = utils_get_u32_le(&req->payload[4]);
    fw_version = utils_get_u32_le(&req->payload[8]);
    image_size = utils_get_u32_le(&req->payload[12]);
    image_crc32 = utils_get_u32_le(&req->payload[16]);
    transfer_id = utils_get_u32_le(&req->payload[20]);
    chunk_size = ota_service_negotiate_chunk_size(utils_get_u32_le(&req->payload[24]));

    if (product_id != BOOT_PRODUCT_ID)
    {
        status = (uint32_t)OTA_ERR_PRODUCT_MISMATCH;
    }
    else if (hw_id != BOOT_FW_HW_ID)
    {
        status = (uint32_t)OTA_ERR_HW_MISMATCH;
    }
    else if ((fw_version == 0U) ||
             ((info->ota_state != (uint32_t)OTA_STATE_REQUEST) &&
              (info->app_valid != 0U) &&
              (info->target_fw_version != 0U) &&
              (fw_version <= info->target_fw_version)))
    {
        status = (uint32_t)OTA_ERR_VERSION_TOO_OLD;
    }
    else if ((image_size < 8U) || (image_size > CONFIG_APP_SIZE))
    {
        status = (uint32_t)OTA_ERR_IMAGE_TOO_LARGE;
    }
    else if ((transfer_id != req->session_id) || (chunk_size == 0U))
    {
        status = (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }
    else if (info->ota_state == (uint32_t)OTA_STATE_VERIFYING)
    {
        status = (uint32_t)OTA_ERR_BUSY;
    }

    if (status == (uint32_t)OTA_OK)
    {
        if (ota_service_start_is_resume(info, transfer_id, fw_version, image_size, image_crc32) != 0U)
        {
            next_offset = info->received_offset;
        }
        else
        {
            new_info = *info;
            new_info.ota_state = (uint32_t)OTA_STATE_DOWNLOADING;
            new_info.image_size = image_size;
            new_info.image_crc32 = image_crc32;
            new_info.received_offset = 0U;
            new_info.transfer_id = transfer_id;
            new_info.target_fw_version = fw_version;
            new_info.fail_reason = (uint32_t)OTA_OK;
            new_info.retry_count = 0U;
            new_info.app_valid = 0U;
            new_info.app_confirmed = 0U;

            if (RET_IS_ERR(ota_eeprom_save(&new_info)))
            {
                status = (uint32_t)OTA_ERR_EEPROM;
            }
            else
            {
                *info = new_info;
            }
        }
    }

    if (status == (uint32_t)OTA_OK)
    {
        s_ota_chunk_size = chunk_size;
        s_ota_transfer_active = 1U;
        next_offset = info->received_offset;
    }
    ota_service_build_start_payload(s_ota_payload_buf, status, transfer_id, (status == (uint32_t)OTA_OK) ? chunk_size : 0U, next_offset);
    ota_service_send_response(req, s_ota_payload_buf, OTA_START_RSP_LEN);
}

/**
 * @brief Validate, write, and acknowledge one OTA_DATA chunk.
 *
 * Validates the transfer state, session, length, alignment, offset, and CRC16.
 * New data is written to Flash; already committed duplicate chunks are
 * acknowledged without rewriting. Progress is checkpointed at page boundaries.
 *
 * @param req  Parsed OTA_DATA request frame.
 * @param info Current OTA runtime information to inspect and update.
 */
static void ota_service_handle_data(const ota_frame_t *req, ota_eeprom_info_t *info)
{
    uint32_t offset = 0U;
    uint32_t previous_offset = info->received_offset;
    uint16_t data_len = 0U;
    uint16_t data_crc16 = 0U;
    uint32_t status = (uint32_t)OTA_OK;
    ota_flash_result_t flash_result;

    if (req->payload_len < OTA_DATA_REQ_HEADER_LEN)
    {
        status = (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }
    else
    {
        offset = utils_get_u32_le(&req->payload[0]);
        data_len = utils_get_u16_le(&req->payload[4]);
        data_crc16 = utils_get_u16_le(&req->payload[6]);
    }

    if (status == (uint32_t)OTA_OK)
    {
        if ((s_ota_transfer_active == 0U) ||
            (info->ota_state != (uint32_t)OTA_STATE_DOWNLOADING) ||
            (req->session_id != info->transfer_id))
        {
            status = (uint32_t)OTA_ERR_NOT_IN_OTA_MODE;
        }
        else if ((data_len == 0U) ||
                 ((uint32_t)data_len > s_ota_chunk_size) ||
                 (req->payload_len !=
                  (uint16_t)(OTA_DATA_REQ_HEADER_LEN + data_len)))
        {
            status = (uint32_t)OTA_ERR_PAYLOAD_LEN;
        }
        else if ((offset > info->image_size) ||
                 ((uint32_t)data_len > (info->image_size - offset)) ||
                 ((offset & 1U) != 0U) ||
                 (((data_len & 1U) != 0U) &&
                  ((offset + data_len) != info->image_size)))
        {
            status = (uint32_t)OTA_ERR_PAYLOAD_LEN;
        }
        else if (utils_calculate_crc16(&req->payload[OTA_DATA_REQ_HEADER_LEN], data_len) != data_crc16)
        {
            status = (uint32_t)OTA_ERR_CHUNK_CRC;
            ota_service_record_recoverable_error(info, status);
        }
        else if (offset < info->received_offset)
        {
            if ((offset + data_len) <= info->received_offset)
            {
                /* A committed duplicate is acknowledged without rewriting. */
                info->fail_reason = (uint32_t)OTA_OK;
            }
            else
            {
                status = (uint32_t)OTA_ERR_OFFSET_MISMATCH;
                ota_service_record_recoverable_error(info, status);
            }
        }
        else if (offset > info->received_offset)
        {
            status = (uint32_t)OTA_ERR_OFFSET_MISMATCH;
            ota_service_record_recoverable_error(info, status);
        }
        else
        {
            flash_result = ota_flash_write_chunk(offset, &req->payload[OTA_DATA_REQ_HEADER_LEN], data_len);
            if (flash_result != OTA_FLASH_OK)
            {
                status = ota_service_flash_result_to_status(flash_result);
                ota_service_record_failure(info, status);
            }
            else
            {
                info->received_offset += data_len;
                info->fail_reason = (uint32_t)OTA_OK;

                if (RET_IS_ERR(ota_service_save_page_checkpoint(info, previous_offset)))
                {
                    status = (uint32_t)OTA_ERR_EEPROM;
                    ota_service_record_failure(info, status);
                }
            }
        }
    }

    ota_service_build_data_payload(s_ota_payload_buf, status, info->received_offset, (uint16_t)(req->seq + 1U));
    ota_service_send_response(req, s_ota_payload_buf, OTA_DATA_RSP_LEN);
}

/**
 * @brief Verify a completed image, persist its result, and reboot into it on success.
 *
 * Confirms the transfer metadata and final offset, saves the VERIFYING state,
 * calculates the application CRC32, and validates the application vector
 * table. On success, it saves SUCCESS, sends the response, then delays and
 * jumps to the application only after the UART transmission is submitted.
 *
 * @param req  Parsed OTA_END request frame.
 * @param info Current OTA runtime information to inspect and update.
 */
static void ota_service_handle_end(const ota_frame_t *req, ota_eeprom_info_t *info)
{
    uint32_t transfer_id = 0U;
    uint32_t image_size = 0U;
    uint32_t image_crc32 = 0U;
    uint32_t image_crc32_calc = 0U;
    uint32_t status = (uint32_t)OTA_OK;
    uint32_t reboot_delay_ms = 0U;
    uint8_t failure_is_terminal = 0U;
    ota_flash_result_t flash_result;
    int tx_ret;
    ota_crc_debug_context_t crc_debug_context;

    if (req->payload_len != OTA_END_REQ_LEN)
    {
        status = (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }
    else
    {
        transfer_id = utils_get_u32_le(&req->payload[0]);
        image_size = utils_get_u32_le(&req->payload[4]);
        image_crc32 = utils_get_u32_le(&req->payload[8]);
    }

    if (status == (uint32_t)OTA_OK)
    {
        if ((s_ota_transfer_active == 0U) ||
            (info->ota_state != (uint32_t)OTA_STATE_DOWNLOADING) ||
            (req->session_id != info->transfer_id))
        {
            status = (uint32_t)OTA_ERR_NOT_IN_OTA_MODE;
        }
        else if ((transfer_id != info->transfer_id) ||
                 (image_size != info->image_size) ||
                 (image_crc32 != info->image_crc32))
        {
            status = (uint32_t)OTA_ERR_IMAGE_INVALID;
        }
        else if (info->received_offset != info->image_size)
        {
            status = (uint32_t)OTA_ERR_OFFSET_MISMATCH;
        }
    }

    if (status == (uint32_t)OTA_OK)
    {
        info->ota_state = (uint32_t)OTA_STATE_VERIFYING;
        if (RET_IS_ERR(ota_eeprom_save(info)))
        {
            status = (uint32_t)OTA_ERR_EEPROM;
            failure_is_terminal = 1U;
        }
    }

    if (status == (uint32_t)OTA_OK)
    {
        crc_debug_context.session_id = req->session_id;
        crc_debug_context.image_size = info->image_size;
        flash_result = ota_flash_calculate_crc32_with_progress(info->image_size, &image_crc32_calc, ota_service_send_crc_debug_event, &crc_debug_context);
        if (flash_result != OTA_FLASH_OK)
        {
            status = ota_service_flash_result_to_status(flash_result);
            failure_is_terminal = 1U;
        }
        else if (image_crc32_calc != info->image_crc32)
        {
            status = (uint32_t)OTA_ERR_IMAGE_CRC;
            failure_is_terminal = 1U;
        }
        else if (boot_app_is_valid() == 0U)
        {
            status = (uint32_t)OTA_ERR_IMAGE_INVALID;
            failure_is_terminal = 1U;
        }
    }

    if (status == (uint32_t)OTA_OK)
    {
        info->ota_state = (uint32_t)OTA_STATE_SUCCESS;
        info->app_valid = 1U;
        info->app_confirmed = 0U;
        info->fail_reason = (uint32_t)OTA_OK;

        if (RET_IS_ERR(ota_eeprom_save(info)))
        {
            status = (uint32_t)OTA_ERR_EEPROM;
            failure_is_terminal = 1U;
        }
        else
        {
            s_ota_transfer_active = 0U;
            reboot_delay_ms = OTA_END_REBOOT_DELAY_MS;
        }
    }

    if (failure_is_terminal != 0U)
    {
        ota_service_record_failure(info, status);
    }

    ota_service_build_end_payload(s_ota_payload_buf, status, image_crc32_calc, reboot_delay_ms);
    tx_ret = ota_service_send_response(req, s_ota_payload_buf, OTA_END_RSP_LEN);
    if ((status == (uint32_t)OTA_OK) && (tx_ret > 0))
    {
        /* Keep interrupts enabled until the UART DMA has completed. */
        delay_ms(reboot_delay_ms);
        boot_jump_to_app();
    }
}

/**
 * @brief Validate and dispatch a parsed OTA request frame.
 *
 * Rejects non-request frames and malformed query payloads. Valid commands are
 * routed to their payload builders or OTA transfer handlers; unknown commands
 * receive an OTA_ERR_UNKNOWN_CMD status response.
 *
 * @param req  Parsed OTA frame to process.
 * @param info Current OTA runtime information.
 */
static void ota_service_handle_request(const ota_frame_t *req, ota_eeprom_info_t *info)
{
    if ((req == NULL) || (info == NULL) || (req->type != (uint8_t)FRAME_TYPE_REQ))
    {
        return;
    }

    if (((req->cmd == (uint16_t)CMD_PING) ||
         (req->cmd == (uint16_t)CMD_GET_INFO) ||
         (req->cmd == (uint16_t)CMD_GET_OTA_STATUS)) &&
        (req->payload_len != 0U))
    {
        ota_service_send_status_response(req, (uint32_t)OTA_ERR_PAYLOAD_LEN);
        return;
    }

    switch (req->cmd)
    {
        case CMD_PING:
        {
            ota_service_build_ping_payload(s_ota_payload_buf, info);
            ota_service_send_response(req, s_ota_payload_buf, OTA_PING_RSP_LEN);
        }
        break;

        case CMD_GET_INFO:
        {
            ota_service_build_get_info_payload(s_ota_payload_buf, info);
            ota_service_send_response(req, s_ota_payload_buf, OTA_GET_INFO_RSP_LEN);
        }
        break;

        case CMD_GET_OTA_STATUS:
        {
            ota_service_build_get_status_payload(s_ota_payload_buf, info);
            ota_service_send_response(req, s_ota_payload_buf, OTA_GET_STATUS_RSP_LEN);
        }
        break;

        case CMD_OTA_START:
        {
            ota_service_handle_start(req, info);
        }
        break;

        case CMD_OTA_DATA:
        {
            ota_service_handle_data(req, info);
        }
        break;

        case CMD_OTA_END:
        {
            ota_service_handle_end(req, info);
        }
        break;

        default:
        {
            ota_service_send_status_response(req, (uint32_t)OTA_ERR_UNKNOWN_CMD);
        }
        break;
    }
}

/**
 * @brief Run the bootloader OTA service loop using supplied or persisted OTA state.
 *
 * Initializes service state, flushes pending UART input, then processes frames
 * indefinitely. A valid header with an invalid frame CRC receives a CRC error
 * response; frames with invalid headers are ignored.
 *
 * @param initial_info       Optional preloaded OTA runtime information.
 * @param initial_info_valid Non-zero when @p initial_info is valid; otherwise
 *                           runtime information is loaded from EEPROM.
 */
void ota_service_run(const ota_eeprom_info_t *initial_info, uint8_t initial_info_valid)
{
    ota_eeprom_info_t info;

    if ((initial_info != NULL) && (initial_info_valid != 0U))
    {
        info = *initial_info;
    }
    else
    {
        ota_service_load_runtime_info(&info);
    }

    s_ota_chunk_size = 0U;
    s_ota_transfer_active = 0U;
    dev_uart_flush_rx(BOARD_UART3_NAME);

    while (1)
    {
        int rx_len = dev_uart_read(BOARD_UART3_NAME, s_ota_rx_buf);

        if (rx_len > 0)
        {
            ota_frame_t frame;
            int parse_ret;

            parse_ret = ota_protocol_parse_frame(s_ota_rx_buf, (uint32_t)rx_len, &frame);
            if (RET_IS_OK(parse_ret))
            {
                ota_service_handle_request(&frame, &info);
            }
            else if ((parse_ret == RET_IO_ERROR) &&
                     RET_IS_OK(ota_protocol_parse_header(s_ota_rx_buf, (uint32_t)rx_len, &frame)) && (frame.type == (uint8_t)FRAME_TYPE_REQ))
            {
                /* The HCRC protects routing fields, so an FCRC error can
                   be reported safely. Damaged headers remain ignored. */
                ota_service_record_recoverable_error(&info, (uint32_t)OTA_ERR_FRAME_CRC);
                ota_service_send_status_response(&frame, (uint32_t)OTA_ERR_FRAME_CRC);
            }
        }
    }
}
