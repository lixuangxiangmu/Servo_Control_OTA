/*****************************************************************************
 * @file    ota_service.c
 * @brief   App-side OTA discovery and bootloader entry service
 *
 * This module implements the application-layer OTA service that runs on the
 * running firmware (the "App" image). It handles two OTA protocol commands:
 *
 *   CMD_GET_INFO  — Reports device identity (product ID, hardware revision),
 *                   currently installed App and Boot firmware versions, flash
 *                   geometry, and supported OTA feature flags (e.g. resume).
 *   CMD_ENTER_OTA — Validates an incoming firmware-update offer by checking
 *                   product/hardware compatibility, version monotonicity, and
 *                   image size against the available flash region. If the
 *                   offer is accepted, the session metadata is persisted to
 *                   EEPROM and the MCU resets into the Boot image, which will
 *                   then conduct the actual data transfer and flash programming.
 *
 * The service communicates over the Bluetooth UART transport. Every response
 * is a properly-formed OTA protocol frame with header and frame CRC-16
 * checksums, built by the shared protocol layer (ota_protocol.c/.h).
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

/* --------------------------------------------------------------------------
 * Application-level OTA constants
 * -------------------------------------------------------------------------- */

/**
 * Product identifier — uniquely identifies this device class to the remote
 * OTA host. The host must supply this exact value in the CMD_ENTER_OTA
 * request, otherwise the update is rejected with OTA_ERR_PRODUCT_MISMATCH.
 * This prevents cross-flashing firmware intended for a different product.
 */
#define APP_OTA_PRODUCT_ID              0x00001001UL

/**
 * Hardware revision identifier — distinguishes between PCB revisions of the
 * same product that may require different firmware binaries. Verified against
 * the host-supplied value during CMD_ENTER_OTA.
 */
#define APP_OTA_HW_ID                   0x00000001UL

/** Fixed payload length of the CMD_GET_INFO response frame (56 bytes). */
#define APP_OTA_GET_INFO_RSP_LEN        56U

/** Expected payload length of the CMD_ENTER_OTA request frame (28 bytes). */
#define APP_OTA_ENTER_REQ_LEN           28U

/** Fixed payload length of the CMD_ENTER_OTA response frame (12 bytes). */
#define APP_OTA_ENTER_RSP_LEN           12U

/** Fixed payload length of a plain status response frame (4 bytes). */
#define APP_OTA_STATUS_RSP_LEN          4U

/**
 * Delay (in milliseconds) between transmitting the CMD_ENTER_OTA acceptance
 * response and issuing the NVIC system reset. This gives the Bluetooth stack
 * time to flush the response onto the wire before the MCU reboots.
 */
#define APP_OTA_REBOOT_DELAY_MS         1000U

/* --------------------------------------------------------------------------
 * Packed version numbers for the GET_INFO response
 *
 * Each version component (major.minor.patch) is packed into a 32-bit integer
 * as {major[15:0], minor[7:0], patch[7:0]}, which is the canonical wire
 * format understood by the remote OTA host.
 * -------------------------------------------------------------------------- */

/** App firmware version packed from CONFIG_APP_VERSION_xxx build-time macros. */
#define APP_OTA_VERSION_NUM \
    ((((uint32_t)CONFIG_APP_VERSION_MAJOR) << 16) | \
     (((uint32_t)CONFIG_APP_VERSION_MINOR) << 8) | \
     ((uint32_t)CONFIG_APP_VERSION_PATCH))

/** Boot firmware version packed from CONFIG_BOOT_VERSION_xxx build-time macros. */
#define APP_OTA_BOOT_VERSION_NUM \
    ((((uint32_t)CONFIG_BOOT_VERSION_MAJOR) << 16) | \
     (((uint32_t)CONFIG_BOOT_VERSION_MINOR) << 8) | \
     ((uint32_t)CONFIG_BOOT_VERSION_PATCH))

/* --------------------------------------------------------------------------
 * Static working buffers — shared across all OTA service calls
 *
 * These are declared at file scope to avoid stack allocation of large
 * buffers (~512 bytes each) and to eliminate the need for dynamic allocation.
 * The service is single-threaded by design (all OTA processing happens in
 * the context of the UART receive task), so no mutual-exclusion guard is
 * required.
 * -------------------------------------------------------------------------- */

/** Transmit buffer for serialized outgoing OTA frames. */
static uint8_t s_ota_tx_buf[OTA_MAX_FRAME_LEN];

/**
 * Payload assembly buffer — reused across GET_INFO, ENTER_OTA, and status
 * response construction.
 */
static uint8_t s_ota_payload_buf[APP_OTA_GET_INFO_RSP_LEN];

/** Monotonically increasing transmit sequence number. */
static uint16_t s_ota_tx_seq;

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * Construct and transmit a response frame over the Bluetooth UART.
 *
 * Builds a full OTA response frame from the supplied payload, using the
 * request frame's CMD and SESSION fields for routing, and transmits it via
 * app_hw_bluetooth_write().
 *
 * @param req          The incoming request frame to which we are responding.
 * @param payload      Pointer to the response payload data (may be NULL if
 *                     payload_len is 0).
 * @param payload_len  Length of the response payload in bytes.
 * @return             RET_OK on success, or a negative error code from the
 *                     protocol layer (frame construction failure) or the
 *                     Bluetooth transport (TX failure).
 */
static int ota_service_send_response(const ota_frame_t *req, const uint8_t *payload, uint16_t payload_len)
{
    ota_frame_t rsp;
    int frame_len;

    /* Populate the response frame descriptor — type, sequence, and
     * routing fields are derived from the request. */
    rsp.type = (uint8_t)FRAME_TYPE_RSP;
    rsp.flags = 0U;
    rsp.seq = s_ota_tx_seq++;
    rsp.ack_seq = req->seq;          /* Echo the request sequence for ACK tracking */
    rsp.cmd = req->cmd;              /* Same command — response semantics are CMD-specific */
    rsp.payload_len = payload_len;
    rsp.session_id = req->session_id;
    rsp.payload = payload;

    /* Serialize the frame descriptor into the transmit buffer */
    frame_len = ota_protocol_build_frame(&rsp, s_ota_tx_buf, sizeof(s_ota_tx_buf));
    if (frame_len < 0)
    {
        return frame_len;
    }

    /* Push the serialized frame out over the Bluetooth UART */
    return app_hw_bluetooth_write(s_ota_tx_buf, (uint32_t)frame_len, DEV_UART_DEFAULT_TIMEOUT_TICKS);
}

/**
 * Transmit a simple 4-byte status-code response.
 *
 * This is a convenience wrapper around ota_service_send_response() for error
 * replies that carry only a 32-bit status code and no additional payload.
 *
 * @param req     The request frame being rejected or acknowledged.
 * @param status  The OTA status code (see ota_status_t) to return.
 * @return        RET_OK on success, or a negative error code on TX failure.
 */
static int ota_service_send_status(const ota_frame_t *req, uint32_t status)
{
    uint8_t payload[APP_OTA_STATUS_RSP_LEN];

    utils_put_u32_le(payload, status);
    return ota_service_send_response(req, payload, sizeof(payload));
}

/**
 * Populate the 56-byte GET_INFO response payload.
 *
 * The payload layout (all fields little-endian) is:
 *   Offset  Size  Description
 *   ------  ----  -------------------------------------------------
 *   0       4     Status code (always OTA_OK for a successful build)
 *   4       4     Product ID
 *   8       4     Hardware revision ID
 *   12      4     Boot firmware version (packed major.minor.patch)
 *   16      4     App firmware version (packed major.minor.patch)
 *   20      4     App base address in flash
 *   24      4     App region size in bytes
 *   28      4     Flash page (sector) size in bytes
 *   32      4     OTA wire-protocol version
 *   36      4     Supported feature bitmask (see OTA_SUPPORT_RESUME, etc.)
 *   40      16    Device serial number (ASCII + zero-padding)
 *
 * @param payload  Output buffer of at least APP_OTA_GET_INFO_RSP_LEN bytes.
 */
static void ota_service_build_info_payload(uint8_t *payload)
{
    /* Human-readable device serial number — 12 ASCII characters + 4 zero pad */
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

/**
 * Validate a CMD_ENTER_OTA request and prepare the persistent OTA session
 * state in EEPROM.
 *
 * Performs the following checks in sequence:
 *   1. Payload length must equal APP_OTA_ENTER_REQ_LEN.
 *   2. Product ID must match APP_OTA_PRODUCT_ID.
 *   3. Hardware revision must match APP_OTA_HW_ID.
 *   4. Target firmware version must be strictly greater than the currently
 *      installed App version (downgrades and no-ops are rejected).
 *   5. Image size must be at least 8 bytes (minimum valid firmware header)
 *      and must not exceed the flash region reserved for the App image.
 *   6. The transfer ID (supplied by the host) must be non-zero and must match
 *      the request's session ID (they are the same value in practice; this
 *      serves as a consistency check).
 *
 * If all checks pass, the existing EEPROM OTA info block is loaded (or
 * initialized to defaults if unreadable), populated with the session
 * parameters, and written back to persistent storage.
 *
 * @param req          The parsed CMD_ENTER_OTA request frame.
 * @param info         Output OTA info block populated on success.
 * @param transfer_id  Output — the host-supplied transfer identifier.
 * @return             OTA_OK on success, or an appropriate OTA error code.
 */
static uint32_t ota_service_prepare_enter(const ota_frame_t *req, ota_eeprom_info_t *info, uint32_t *transfer_id)
{
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t target_fw_version;
    uint32_t image_size;
    uint32_t image_crc32;

    /* --- Payload length check: must exactly match the expected request size --- */
    if (req->payload_len != APP_OTA_ENTER_REQ_LEN)
    {
        return (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }

    /* --- Extract fields from the request payload (all little-endian) --- */
    product_id = utils_get_u32_le(&req->payload[0]);
    hw_id = utils_get_u32_le(&req->payload[4]);
    target_fw_version = utils_get_u32_le(&req->payload[8]);
    image_size = utils_get_u32_le(&req->payload[12]);
    image_crc32 = utils_get_u32_le(&req->payload[16]);
    *transfer_id = utils_get_u32_le(&req->payload[20]);

    /* --- Identity checks: reject mismatched product or hardware revision --- */
    if (product_id != APP_OTA_PRODUCT_ID)
    {
        return (uint32_t)OTA_ERR_PRODUCT_MISMATCH;
    }
    if (hw_id != APP_OTA_HW_ID)
    {
        return (uint32_t)OTA_ERR_HW_MISMATCH;
    }

    /* --- Version check: target must be strictly newer than current --- */
    if ((target_fw_version == 0U) || (target_fw_version <= APP_OTA_VERSION_NUM))
    {
        return (uint32_t)OTA_ERR_VERSION_TOO_OLD;
    }

    /* --- Image size check: must fit within the App flash partition --- */
    if ((image_size < 8U) || (image_size > CONFIG_APP_SIZE))
    {
        return (uint32_t)OTA_ERR_IMAGE_TOO_LARGE;
    }

    /* --- Transfer-ID sanity check: non-zero and consistent with session ID --- */
    if ((*transfer_id == 0U) || (*transfer_id != req->session_id))
    {
        return (uint32_t)OTA_ERR_PAYLOAD_LEN;
    }

    /* --- Load or initialize the persistent OTA info block --- */
    if (RET_IS_ERR(ota_eeprom_load(info)))
    {
        ota_eeprom_init_default(info);
    }

    /* --- Populate session parameters --- */
    info->ota_state = (uint32_t)OTA_STATE_REQUEST;
    info->image_size = image_size;
    info->image_crc32 = image_crc32;
    info->received_offset = 0U;           /* Transfer starts from byte 0 */
    info->transfer_id = *transfer_id;
    info->target_fw_version = target_fw_version;
    info->fail_reason = (uint32_t)OTA_OK; /* Clear any prior failure reason */
    info->retry_count = 0U;               /* This is a fresh session */

    /* --- Persist to EEPROM so the Boot image can read it after reset --- */
    if (RET_IS_ERR(ota_eeprom_save(info)))
    {
        return (uint32_t)OTA_ERR_EEPROM;
    }

    return (uint32_t)OTA_OK;
}

/**
 * Full handler for the CMD_ENTER_OTA request.
 *
 * Calls ota_service_prepare_enter() to validate and persist the session.
 * Constructs the 12-byte response payload:
 *   [0..3]   Status code
 *   [4..7]   Transfer ID (echoed back to the host for correlation)
 *   [8..11]  Reboot delay in milliseconds (non-zero only on acceptance)
 *
 * On success (OTA_OK), the function delays for APP_OTA_REBOOT_DELAY_MS to
 * allow the Bluetooth stack to flush the response, then triggers an NVIC
 * system reset. The Boot image will detect the OTA_STATE_REQUEST in EEPROM
 * and enter its data-transfer phase.
 *
 * @param req  The parsed CMD_ENTER_OTA request frame.
 * @return     RET_OK if the response was accepted and the reset was
 *             initiated; RET_FAIL or a negative error code otherwise.
 */
static int ota_service_handle_enter(const ota_frame_t *req)
{
    ota_eeprom_info_t info;
    uint32_t transfer_id = 0U;
    uint32_t status;
    int ret;

    /* Validate and persist the OTA session */
    status = ota_service_prepare_enter(req, &info, &transfer_id);

    /* Build the 12-byte response payload */
    utils_put_u32_le(&s_ota_payload_buf[0], status);
    utils_put_u32_le(&s_ota_payload_buf[4], transfer_id);
    /* Only include the reboot delay when the request was accepted */
    utils_put_u32_le(&s_ota_payload_buf[8], (status == (uint32_t)OTA_OK) ? APP_OTA_REBOOT_DELAY_MS : 0U);

    /* Transmit the response before resetting */
    ret = ota_service_send_response(req, s_ota_payload_buf, APP_OTA_ENTER_RSP_LEN);

    if ((status == (uint32_t)OTA_OK) && RET_IS_OK(ret))
    {
        /* The response has been accepted and successfully transmitted.
         * Wait for the Bluetooth stack to flush the TX buffer, then reset
         * into the Boot image to begin the data-transfer phase. */
        vTaskDelay(pdMS_TO_TICKS(APP_OTA_REBOOT_DELAY_MS));
        NVIC_SystemReset();
    }

    /* Return the transport result if the transmission itself failed,
     * otherwise return the logical status of the enter request. */
    return RET_IS_ERR(ret) ? ret : ((status == (uint32_t)OTA_OK) ? RET_OK : RET_FAIL);
}

/* --------------------------------------------------------------------------
 * Public API implementation
 * -------------------------------------------------------------------------- */

/**
 * @see ota_service.h for the full contract.
 *
 * This is the primary triage point for incoming UART data. It is designed to
 * be called on every received Bluetooth packet before any other payload
 * interpretation. The check is intentionally minimal — it only inspects the
 * first two bytes — so that non-OTA traffic suffers negligible overhead.
 */
uint8_t ota_service_is_protocol_frame(const uint8_t *buf, uint32_t len)
{
    if ((buf == NULL) || (len < 2U))
    {
        return 0U;
    }

    return ((buf[0] == OTA_SOF0) && (buf[1] == OTA_SOF1)) ? 1U : 0U;
}

/**
 * @see ota_service.h for the full contract.
 *
 * Top-level dispatch: every OTA request frame arrives here after passing the
 * SOF check in ota_service_is_protocol_frame(). The function parses the
 * frame, verifies it is a request (not a response), and routes it to the
 * handler matching the CMD field.
 *
 * For unrecognized commands, an OTA_ERR_UNKNOWN_CMD status response is
 * transmitted so the remote host can distinguish a dead device from an
 * unsupported operation.
 */
int ota_service_process_frame(const uint8_t *buf, uint32_t len)
{
    ota_frame_t req;
    int ret;

    /* Parse and validate the raw frame (structural + CRC checks) */
    ret = ota_protocol_parse_frame(buf, len, &req);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Only request frames are processed by this service */
    if (req.type != (uint8_t)FRAME_TYPE_REQ)
    {
        return RET_INVALID_PARAM;
    }

    switch (req.cmd)
    {
        case CMD_GET_INFO:
        {
            /* GET_INFO expects an empty payload — any data is a protocol error */
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
            /* Unknown command — inform the host with a status response */
            return ota_service_send_status(&req, (uint32_t)OTA_ERR_UNKNOWN_CMD);
        }
    }
}
