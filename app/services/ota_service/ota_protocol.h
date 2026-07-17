#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H

#include <stdint.h>

/* --------------------------------------------------------------------------
 * Frame-level protocol constants
 * -------------------------------------------------------------------------- */

/** Start-of-frame delimiter byte 0 — must appear at offset 0 of every frame. */
#define OTA_SOF0                    0x55U
/** Start-of-frame delimiter byte 1 — must appear at offset 1 of every frame. */
#define OTA_SOF1                    0xAAU
/** Current wire-protocol version encoded in each frame header. */
#define OTA_PROTO_VER               0x01U
/** Fixed-size header length in bytes (excluding SOF and the header CRC). */
#define OTA_HEADER_LEN              20U
/** Trailing frame-level CRC length in bytes (CCITT CRC-16, little-endian). */
#define OTA_FRAME_CRC_LEN           2U
/** Absolute minimum frame size: header + trailing CRC (zero-length payload). */
#define OTA_FRAME_MIN_LEN           (OTA_HEADER_LEN + OTA_FRAME_CRC_LEN)
/** Maximum payload bytes allowed in a single OTA data frame. */
#define OTA_MAX_PAYLOAD             490U
/** Worst-case frame buffer size covering a full payload + framing overhead. */
#define OTA_MAX_FRAME_LEN           (OTA_HEADER_LEN + OTA_MAX_PAYLOAD + OTA_FRAME_CRC_LEN)

/* --------------------------------------------------------------------------
 * Feature capability bitmask — reported in GET_INFO response
 * -------------------------------------------------------------------------- */

/**
 * Bit 0: resume-from-breakpoint is supported by this Boot image.
 * The remote client may skip already-transferred chunks during the
 * subsequent data-transfer phase.
 */
#define OTA_SUPPORT_RESUME          (1UL << 0)

/* --------------------------------------------------------------------------
 * Frame type enumeration
 * -------------------------------------------------------------------------- */

/**
 * Semantics of the TYPE field in the frame header.
 * Every wire-level message is either a request (initiated by the remote host)
 * or a response (sent by the device).
 */
typedef enum
{
    /** Frame sent by the remote host to initiate an operation. */
    FRAME_TYPE_REQ = 0x01U,
    /** Frame sent by the device in reply to a request. */
    FRAME_TYPE_RSP = 0x02U,
} ota_frame_type_t;

/* --------------------------------------------------------------------------
 * Command identifier enumeration
 * -------------------------------------------------------------------------- */

/**
 * Application-layer command codes carried in the CMD field of the frame
 * header. The device (App-side) only processes CMD_GET_INFO and CMD_ENTER_OTA;
 * other commands are rejected with OTA_ERR_UNKNOWN_CMD.
 */
typedef enum
{
    /** Query device identity, firmware versions, and flash geometry. */
    CMD_GET_INFO  = 0x0002U,
    /** Request the device to accept a firmware transfer and reboot into Boot. */
    CMD_ENTER_OTA = 0x0004U,
} ota_cmd_t;

/* --------------------------------------------------------------------------
 * Status / error code enumeration
 * -------------------------------------------------------------------------- */

/**
 * Status codes returned in the payload of a response frame.
 * Codes in the 0x01xx range indicate a mismatch between the host-supplied
 * image metadata and the device's own identity; codes in 0x02xx indicate
 * storage or hardware-related failures.
 */
typedef enum
{
    /** Operation completed successfully. */
    OTA_OK                       = 0x0000U,
    /** The CMD field did not match any known command on this device. */
    OTA_ERR_UNKNOWN_CMD          = 0x0001U,
    /** The payload length did not match the expected length for this command. */
    OTA_ERR_PAYLOAD_LEN          = 0x0003U,
    /** The product ID in the ENTER_OTA request does not match this device. */
    OTA_ERR_PRODUCT_MISMATCH     = 0x0100U,
    /** The hardware revision ID in the ENTER_OTA request does not match. */
    OTA_ERR_HW_MISMATCH          = 0x0101U,
    /** The target firmware version is not newer than the currently installed version. */
    OTA_ERR_VERSION_TOO_OLD      = 0x0102U,
    /** The declared image size exceeds the available application flash region. */
    OTA_ERR_IMAGE_TOO_LARGE      = 0x0103U,
    /** Read or write of the OTA persistent-state area in EEPROM/Flash failed. */
    OTA_ERR_EEPROM               = 0x0206U,
} ota_status_t;

/* --------------------------------------------------------------------------
 * Frame descriptor structure — used by both parse and build paths
 * -------------------------------------------------------------------------- */

/**
 * In-memory representation of a single OTA protocol frame.
 *
 * This struct is populated by ota_protocol_parse_frame() when receiving a
 * frame and consumed by ota_protocol_build_frame() when constructing one.
 * The payload pointer references the original buffer on parse and is copied
 * into the output buffer on build — callers must keep the source buffer alive
 * until the build completes.
 */
typedef struct
{
    /** Frame type — see ota_frame_type_t. */
    uint8_t type;
    /** Per-frame flags (reserved for future use; currently always 0). */
    uint8_t flags;
    /** Monotonically-increasing per-direction sequence number. */
    uint16_t seq;
    /** Sequence number of the frame being acknowledged (valid in response frames). */
    uint16_t ack_seq;
    /** Application-layer command — see ota_cmd_t. */
    uint16_t cmd;
    /** Length of the payload buffer in bytes (0 when no payload is present). */
    uint16_t payload_len;
    /**
     * Session identifier — ties a CMD_ENTER_OTA request to the subsequent
     * data-transfer session in the Boot image.
     */
    uint32_t session_id;
    /**
     * Pointer to the payload data. May be NULL when payload_len is 0.
     * On parse, this points into the receive buffer; on build, the caller
     * provides a pointer to the payload to be copied out.
     */
    const uint8_t *payload;
} ota_frame_t;

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * Parse a raw byte buffer into a structured ota_frame_t descriptor.
 *
 * Performs the following validations in order:
 *   1. NULL-pointer guard on buf and frame.
 *   2. Minimum length check against OTA_FRAME_MIN_LEN.
 *   3. Magic-number match (SOF0 / SOF1), protocol version, and header-length field.
 *   4. Declared payload length does not exceed OTA_MAX_PAYLOAD.
 *   5. Total wire length (header + payload + CRC) matches the supplied buffer length.
 *   6. Header CRC-16 (HC — covers VER through SESSION, inclusive).
 *   7. Frame CRC-16 (FC — covers VER through end of payload, inclusive).
 *
 * @param buf    Pointer to the raw receive buffer containing a complete frame.
 * @param len    Number of bytes available in buf.
 * @param frame  Output descriptor populated on success; contents are undefined
 *               on failure.
 * @return       RET_OK on success, or a negative error code:
 *               - RET_INVALID_PARAM  – null pointer or length mismatch.
 *               - RET_BUFFER_TOO_SMALL – buf shorter than minimum frame size
 *                 or declared payload exceeds the protocol maximum.
 *               - RET_FAIL           – magic/version/header-length mismatch.
 *               - RET_IO_ERROR       – CRC mismatch (either HC or FC).
 */
int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame);

/**
 * Serialize a frame descriptor into a wire-format byte buffer.
 *
 * Writes SOF, header fields, payload (if any), HC, and FC in little-endian
 * order. The output buffer must be large enough to hold the full frame
 * (OTA_HEADER_LEN + payload_len + OTA_FRAME_CRC_LEN bytes).
 *
 * @param frame    Descriptor containing the fields to encode.
 * @param out      Output buffer to receive the serialized frame.
 * @param out_size Capacity of the output buffer in bytes.
 * @return         The total number of bytes written on success, or a negative
 *                 error code:
 *                 - RET_INVALID_PARAM  – null pointer or payload pointer absent
 *                   when payload_len > 0.
 *                 - RET_BUFFER_TOO_SMALL – out_size insufficient for the frame.
 */
int ota_protocol_build_frame(const ota_frame_t *frame,
                             uint8_t *out,
                             uint32_t out_size);

#endif /* OTA_PROTOCOL_H */
