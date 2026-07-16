#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Protocol Constants
 *===========================================================================*/

/** Start-of-Frame byte 0: magic number 0x55 for frame boundary detection. */
#define OTA_SOF0                    0x55U
/** Start-of-Frame byte 1: magic number 0xAA for frame boundary detection. */
#define OTA_SOF1                    0xAAU
/** Current protocol version. Incremented when the wire format changes
    incompatibly. */
#define OTA_PROTO_VER               0x01U
/** Fixed header length in bytes. All frames have exactly this many header
    bytes before the payload begins. */
#define OTA_HEADER_LEN              20U
/** Length of each CRC-16 field in bytes (2 bytes per CRC). */
#define OTA_FRAME_CRC_LEN           2U
/** Minimum valid frame length: header + trailing FCRC, with zero payload. */
#define OTA_FRAME_MIN_LEN           (OTA_HEADER_LEN + OTA_FRAME_CRC_LEN)
/** Maximum payload size per frame (chosen to fit within a single UART DMA
    buffer less protocol overhead). */
#define OTA_MAX_PAYLOAD             490U
/** Maximum possible frame length: header + max payload + FCRC. */
#define OTA_MAX_FRAME_LEN           (OTA_HEADER_LEN + OTA_MAX_PAYLOAD + OTA_FRAME_CRC_LEN)

/*===========================================================================
 * Capability / Feature Flags (used in the FLAGS header byte)
 *===========================================================================*/

/** Device supports resuming an interrupted OTA transfer from the last
    successfully written offset. */
#define OTA_SUPPORT_RESUME          (1UL << 0)
/** Device supports payload-level encryption for secure firmware delivery. */
#define OTA_SUPPORT_ENCRYPT         (1UL << 1)
/** Device supports digital signature verification of the firmware image. */
#define OTA_SUPPORT_SIGN            (1UL << 2)
/** Device supports on-the-fly decompression of compressed firmware images. */
#define OTA_SUPPORT_COMPRESS        (1UL << 3)
/** Device supports A/B (dual-bank) partition scheme for safe fallback
    after a failed update. */
#define OTA_SUPPORT_AB              (1UL << 4)

/*===========================================================================
 * Frame Types
 *
 * Each frame on the wire carries a type field that determines how the
 * receiver should interpret the frame.
 *===========================================================================*/

/**
 * @brief Enumerates the possible OTA frame types.
 *
 * The protocol uses five frame types to implement a reliable request-
 * response-acknowledge communication pattern:
 *
 *   Host                     Device
 *    |  ---- REQ (CMD) ----->  |   Host sends a command request.
 *    |  <--- RSP (DATA) -----  |   Device responds with data.
 *    |  ---- ACK (SEQ) ----->  |   Host acknowledges receipt.
 *    |  <--- NACK (ERR) -----  |   Device reports an error.
 *    |  <--- EVENT ----------  |   Device pushes an unsolicited event.
 */
typedef enum
{
    /** Request frame sent by the host to initiate a command. */
    FRAME_TYPE_REQ   = 0x01U,
    /** Response frame sent by the device carrying command result data. */
    FRAME_TYPE_RSP   = 0x02U,
    /** Positive acknowledgement frame: confirms successful reception. */
    FRAME_TYPE_ACK   = 0x03U,
    /** Negative acknowledgement frame: signals an error condition. */
    FRAME_TYPE_NACK  = 0x04U,
    /** Unsolicited event frame pushed by the device (e.g., status change). */
    FRAME_TYPE_EVENT = 0x05U,
} ota_frame_type_t;

/*===========================================================================
 * Command Identifiers
 *
 * The CMD field specifies which operation the host is requesting or which
 * operation a response relates to. Commands are grouped logically:
 *   0x0001 ~ 0x000F: General / system commands
 *   0x0010 ~ 0x001F: OTA data transfer commands
 *   0x0020 ~ 0x002F: Parameter read/write commands
 *   0x8000 ~        : Vendor-specific extensions
 *===========================================================================*/

typedef enum
{
    /* ---- General / System Commands (0x0001 ~ 0x000F) ---- */

    /** Ping request: device responds with protocol version, bootloader
        version, and application validity status. Used for device discovery
        and connectivity check. */
    CMD_PING             = 0x0001U,
    /** Get device info: device returns product ID, hardware ID, firmware
        versions, flash layout parameters, device serial number, and
        supported features. */
    CMD_GET_INFO         = 0x0002U,
    /** Get current OTA transfer status: device returns the state of an
        in-progress or interrupted OTA session (transfer ID, image size,
        received offset, failure reason, etc.). */
    CMD_GET_OTA_STATUS   = 0x0003U,
    /** Enter OTA mode: instructs the device to switch from normal operation
        into the OTA update service. The device must already be in the
        bootloader for OTA commands to be accepted. */
    CMD_ENTER_OTA        = 0x0004U,
    /** Reboot command: instructs the device to perform a software reset.
        Typically sent after a successful OTA update to boot the new
        firmware. */
    CMD_REBOOT           = 0x0005U,

    /* ---- OTA Data Transfer Commands (0x0010 ~ 0x001F) ---- */

    /** Start a new OTA transfer session. The payload includes firmware image
        metadata (total size, CRC32, target version). The device validates
        compatibility (product ID, hardware ID, version, available space)
        and initializes the flash write context. */
    CMD_OTA_START        = 0x0010U,
    /** Transfer a chunk of firmware image data. The payload contains a slice
        of the binary firmware at the expected offset. The device writes the
        data to flash and optionally verifies the write via readback. */
    CMD_OTA_DATA         = 0x0011U,
    /** End the current OTA transfer. The host signals that all data has been
        sent. The device performs final verification (full image CRC32
        check), marks the application as valid, and updates EEPROM state. */
    CMD_OTA_END          = 0x0012U,
    /** Abort the current OTA transfer. Discards the incomplete session and
        rolls back any partially written data. The device clears its transfer
        state and returns to idle. */
    CMD_OTA_ABORT        = 0x0013U,
    /** Verify the written firmware image without ending the session. The
        device reads back the written data and compares the CRC32. Useful
        as a pre-commit check before OTA_END. */
    CMD_OTA_VERIFY       = 0x0014U,
    /** Resume a previously interrupted OTA transfer. The host provides the
        transfer ID of the incomplete session. The device reports the
        last successfully written offset so the host can continue sending
        data from that point. */
    CMD_OTA_RESUME       = 0x0015U,

    /* ---- Parameter Read/Write Commands (0x0020 ~ 0x002F) ---- */

    /** Read a configuration parameter from the device (e.g., baud rate,
        device name, calibration values). */
    CMD_READ_PARAM       = 0x0020U,
    /** Write a configuration parameter to the device's persistent storage. */
    CMD_WRITE_PARAM      = 0x0021U,

    /* ---- Vendor-Specific Extensions ---- */

    /** Start of vendor-defined command range. Commands at or above this
        value are application-specific and not defined by the protocol. */
    CMD_VENDOR_BEGIN     = 0x8000U,
} ota_cmd_t;

/*===========================================================================
 * Status / Error Codes
 *
 * Every response frame includes a 32-bit status code in the first 4 bytes
 * of its payload. These codes are grouped by category to aid debugging:
 *   0x00xx: Protocol-level errors
 *   0x01xx: Image validation / compatibility errors
 *   0x02xx: Data transfer / flash operation errors
 *   0x03xx: Runtime errors
 *===========================================================================*/

typedef enum
{
    /* ---- Success ---- */
    /** Operation completed successfully with no errors. */
    OTA_OK                       = 0x0000U,

    /* ---- Protocol-Level Errors (0x0001 ~ 0x00FF) ---- */

    /** The received command identifier is not recognized by this device. */
    OTA_ERR_UNKNOWN_CMD          = 0x0001U,
    /** Frame CRC-16 check failed. The received data is corrupted or the frame was truncated during transmission. */
    OTA_ERR_FRAME_CRC            = 0x0002U,
    /** Payload length exceeds the maximum allowed size or is inconsistent with the expected length for the given command. */
    OTA_ERR_PAYLOAD_LEN          = 0x0003U,
    /** The device is currently processing a previous command and cannot accept a new request. 
     * The host should retry after a short delay. */
    OTA_ERR_BUSY                 = 0x0004U,
    /** The device is not in OTA mode. OTA commands are only accepted in the bootloader, 
     * not during normal application execution. The host must first trigger entry into the bootloader. */
    OTA_ERR_NOT_IN_OTA_MODE      = 0x0005U,

    /* ---- Image Validation / Compatibility Errors (0x0100 ~ 0x01FF) ---- */

    /** Product ID in the firmware image does not match the device's product
        ID. The firmware was built for a different product. */
    OTA_ERR_PRODUCT_MISMATCH     = 0x0100U,
    /** Hardware revision ID mismatch. The firmware requires different hardware than what is present on this device. */
    OTA_ERR_HW_MISMATCH          = 0x0101U,
    /** The firmware version in the image is older than or equal to the currently installed version. 
     * Downgrade prevention. */
    OTA_ERR_VERSION_TOO_OLD      = 0x0102U,
    /** The firmware image size exceeds the available application flash partition space. */
    OTA_ERR_IMAGE_TOO_LARGE      = 0x0103U,
    /** Full-image CRC32 verification failed at the end of transfer. The image is corrupted or was not transmitted correctly. */
    OTA_ERR_IMAGE_CRC            = 0x0104U,
    /** The firmware image is structurally invalid (e.g., bad header, missing vector table, invalid magic number). */
    OTA_ERR_IMAGE_INVALID        = 0x0105U,

    /* ---- Data Transfer / Flash Errors (0x0200 ~ 0x02FF) ---- */

    /** The payload offset in a CMD_OTA_DATA frame does not match the expected next write address. 
     * Indicates out-of-order or missing data chunks. */
    OTA_ERR_OFFSET_MISMATCH      = 0x0200U,
    /** Sequence number mismatch. The received SEQ does not match the expected next sequence number, 
     * indicating a lost or duplicated frame. */
    OTA_ERR_SEQ_MISMATCH         = 0x0201U,
    /** Per-chunk CRC check failed for the data in CMD_OTA_DATA. The chunk payload is corrupted. */
    OTA_ERR_CHUNK_CRC            = 0x0202U,
    /** Flash sector/page erase operation failed. The target flash area could not be prepared for writing. */
    OTA_ERR_FLASH_ERASE          = 0x0203U,
    /** Flash write operation failed. The data could not be programmed into the application flash region. */
    OTA_ERR_FLASH_WRITE          = 0x0204U,
    /** Flash readback verification failed immediately after writing. The data read back does not match what was written, 
     * indicating a hardware-level flash fault. */
    OTA_ERR_FLASH_READBACK       = 0x0205U,
    /** EEPROM (emulated in flash) read or write operation failed. OTA state persistence could not be saved or loaded. */
    OTA_ERR_EEPROM               = 0x0206U,

    /* ---- Runtime Errors (0x0300 ~ 0x03FF) ---- */

    /** Operation timed out. The device did not receive the expected next frame within the protocol timeout period. */
    OTA_ERR_TIMEOUT              = 0x0300U,
    /** The current OTA operation was explicitly aborted by the host via CMD_OTA_ABORT. */
    OTA_ERR_ABORTED              = 0x0301U,
} ota_status_t;

/**
 * @brief Describes a parsed OTA protocol frame.
 *
 * After a raw UART buffer is successfully parsed by ota_protocol_parse_frame(),
 * the extracted fields are stored in this structure. Note that the payload
 * pointer points into the original receive buffer and is valid only as long
 * as that buffer is not overwritten.
 */
typedef struct
{
    /** Frame type (see ota_frame_type_t: REQ, RSP, ACK, NACK, EVENT). */
    uint8_t type;
    /** Capability / control flags bitmask (see OTA_SUPPORT_* defines). */
    uint8_t flags;
    /** Sequence number of this frame (host and device maintain independentsequence counters). */
    uint16_t seq;
    /** Sequence number being acknowledged. Meaningful in ACK/NACK/RSP frames; set to 0 in REQ frames. */
    uint16_t ack_seq;
    /** Command identifier (see ota_cmd_t). */
    uint16_t cmd;
    /** Length of the payload in bytes (0 if no payload). */
    uint16_t payload_len;
    /** Session identifier binding this frame to a specific OTA session. */
    uint32_t session_id;
    /** Pointer to the payload data within the original receive buffer.NULL if payload_len is 0. Do not free this pointer. */
    const uint8_t *payload;
} ota_frame_t;

/*===========================================================================
 * API Functions
 *===========================================================================*/

int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame);

/**
 * Parse a structurally valid frame header without checking the trailing FCRC.
 * This permits a safe OTA_ERR_FRAME_CRC response only when routing fields
 * are protected by a valid HCRC.
 */
int ota_protocol_parse_header(const uint8_t *buf,
                              uint32_t len,
                              ota_frame_t *frame);

int ota_protocol_build_frame(const ota_frame_t *frame, uint8_t *out, uint32_t out_size);


#ifdef __cplusplus
}
#endif

#endif /* OTA_PROTOCOL_H */
