/*****************************************************************************
 * @file    ota_protocol.c
 * @brief   OTA wire-protocol frame serialization and deserialization
 *
 * This module implements the binary framing layer shared by the App-side OTA
 * service and the Boot-side OTA engine. Every frame on the wire has the
 * following structure (all multi-byte fields are little-endian):
 *
 *   Offset  Size  Field
 *   ------  ----  -------------------------------------------
 *   0       1     SOF0           (fixed 0x55)
 *   1       1     SOF1           (fixed 0xAA)
 *   2       1     Version        (wire-protocol version, currently 0x01)
 *   3       1     Type           (FRAME_TYPE_REQ or FRAME_TYPE_RSP)
 *   4       1     Flags          (reserved, always 0)
 *   5       1     Header Length  (always 20)
 *   6       2     Sequence       (per-direction monotonic counter)
 *   8       2     ACK Sequence   (echoes the seq of the frame being acked)
 *   10      2     Command        (application-layer command code)
 *   12      2     Payload Length (0..490 bytes)
 *   14      4     Session ID     (unique per-OTA-session identifier)
 *   18      2     Header CRC     (CCITT CRC-16 of bytes [2..18))
 *   20      N     Payload        (variable-length, 0..490 bytes)
 *   20+N    2     Frame CRC      (CCITT CRC-16 of bytes [2..20+N))
 *
 * Two CRC-16 checksums protect the frame:
 *   HC (Header CRC)  — covers the Version through Session-ID fields.
 *   FC (Frame CRC)   — covers Version through the end of the payload.
 * The SOF delimiter bytes are deliberately excluded from both CRCs so that
 * the SOF can be used as a synchronization marker without affecting integrity
 * verification.
 *****************************************************************************/

#include "ota_protocol.h"

#include "return_code.h"
#include "utils_lib.h"

#include <stddef.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Wire-format byte offsets within a raw frame buffer
 *
 * These offsets are measured from the start of a complete frame (i.e., from
 * the SOF0 byte). They are used by both parse and build paths to avoid magic
 * numbers scattered through the code.
 * -------------------------------------------------------------------------- */

/** Offset of start-of-frame delimiter byte 0. */
#define OTA_OFFSET_SOF0              0U
/** Offset of start-of-frame delimiter byte 1. */
#define OTA_OFFSET_SOF1              1U
/** Offset of the wire-protocol version field. */
#define OTA_OFFSET_VER               2U
/** Offset of the frame type field (REQ or RSP). */
#define OTA_OFFSET_TYPE              3U
/** Offset of the flags field (currently reserved). */
#define OTA_OFFSET_FLAGS             4U
/** Offset of the header-length field (must equal OTA_HEADER_LEN). */
#define OTA_OFFSET_HLEN              5U
/** Offset of the 16-bit sequence number (little-endian). */
#define OTA_OFFSET_SEQ               6U
/** Offset of the 16-bit ACK sequence number (little-endian). */
#define OTA_OFFSET_ACK_SEQ           8U
/** Offset of the 16-bit command code (little-endian). */
#define OTA_OFFSET_CMD               10U
/** Offset of the 16-bit payload length (little-endian). */
#define OTA_OFFSET_PAYLOAD_LEN       12U
/** Offset of the 32-bit session identifier (little-endian). */
#define OTA_OFFSET_SESSION           14U
/** Offset of the 16-bit header CRC (HC, little-endian). */
#define OTA_OFFSET_HCRC              18U
/** Offset of the variable-length payload data. */
#define OTA_OFFSET_PAYLOAD           20U

/* --------------------------------------------------------------------------
 * CRC coverage ranges
 *
 * HC covers bytes from VER (inclusive) to HCRC (exclusive), i.e. the fixed
 * header fields only. FC covers bytes from VER (inclusive) through the last
 * byte of the payload (inclusive), i.e. the header fields plus payload data.
 * The SOF bytes are excluded from both to allow them to serve as a
 * synchronization marker independently of the CRC computation.
 * -------------------------------------------------------------------------- */

/** Starting offset for the header CRC calculation (skips SOF0 and SOF1). */
#define OTA_HCRC_CALC_OFFSET         OTA_OFFSET_VER
/** Number of bytes covered by the header CRC. */
#define OTA_HCRC_CALC_LEN            (OTA_OFFSET_HCRC - OTA_OFFSET_VER)
/** Starting offset for the frame CRC calculation (same as HC start). */
#define OTA_FCRC_CALC_OFFSET         OTA_OFFSET_VER
/** Base length of the frame CRC coverage — the fixed header portion only. */
#define OTA_FCRC_BASE_LEN            (OTA_HEADER_LEN - OTA_OFFSET_VER)

/* --------------------------------------------------------------------------
 * Internal CRC helpers
 * -------------------------------------------------------------------------- */

/**
 * Compute the header CRC (HC) for a raw frame buffer.
 *
 * The HC covers the fixed-length header fields from VER through SESSION
 * (inclusive). It is placed at offset OTA_OFFSET_HCRC in the wire format
 * and verified by the receiver before the payload is interpreted.
 *
 * @param buf  Pointer to the start of a raw frame buffer (at SOF0).
 * @return     16-bit CCITT CRC-16 computed over the header region.
 */
static uint16_t ota_protocol_calc_hcrc(const uint8_t *buf)
{
    return utils_calculate_crc16(&buf[OTA_HCRC_CALC_OFFSET], OTA_HCRC_CALC_LEN);
}

/**
 * Compute the frame CRC (FC) for a raw frame buffer.
 *
 * The FC covers the header fields (same range as HC) plus the entire variable-
 * length payload. This provides end-to-end integrity protection for the whole
 * frame beyond just the fixed header.
 *
 * @param buf          Pointer to the start of a raw frame buffer (at SOF0).
 * @param payload_len  Length of the payload already written into buf.
 * @return             16-bit CCITT CRC-16 computed over header + payload.
 */
static uint16_t ota_protocol_calc_fcrc(const uint8_t *buf, uint16_t payload_len)
{
    return utils_calculate_crc16(&buf[OTA_FCRC_CALC_OFFSET], (uint16_t)(OTA_FCRC_BASE_LEN + payload_len));
}

/* --------------------------------------------------------------------------
 * Public API implementation
 * -------------------------------------------------------------------------- */

/**
 * @see ota_protocol.h for the full contract.
 *
 * Implementation notes:
 * - The function validates fields in a specific order designed to fail fast:
 *   structural checks (SOF, version, header length) come before length
 *   validation, which comes before CRC checks. This avoids expensive CRC
 *   computation on frames that are structurally invalid.
 * - The payload pointer in the output frame descriptor points directly into
 *   the receive buffer — no copy is performed. The caller must not free or
 *   overwrite the buffer while the descriptor is in use.
 */
int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame)
{
    uint16_t payload_len;
    uint32_t total_len;

    /* --- Guard: reject null pointers before any access --- */
    if ((buf == NULL) || (frame == NULL))
    {
        return RET_INVALID_PARAM;
    }

    /* --- Minimum-length check: must be at least header + trailing CRC --- */
    if (len < OTA_FRAME_MIN_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* --- Structural field validation: SOF magic, protocol version, header length --- */
    if ((buf[OTA_OFFSET_SOF0] != OTA_SOF0) ||
        (buf[OTA_OFFSET_SOF1] != OTA_SOF1) ||
        (buf[OTA_OFFSET_VER] != OTA_PROTO_VER) ||
        (buf[OTA_OFFSET_HLEN] != OTA_HEADER_LEN))
    {
        return RET_FAIL;
    }

    /* --- Payload-length validation: must not exceed the protocol maximum --- */
    payload_len = utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD_LEN]);
    if (payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* --- Total-length validation: buffer length must exactly match the
     *     declared frame size (header + payload + CRC) --- */
    total_len = OTA_HEADER_LEN + (uint32_t)payload_len + OTA_FRAME_CRC_LEN;
    if (len != total_len)
    {
        return RET_INVALID_PARAM;
    }

    /* --- Integrity checks: validate both HC (header) and FC (header+payload) --- */
    if (utils_get_u16_le(&buf[OTA_OFFSET_HCRC]) != ota_protocol_calc_hcrc(buf))
    {
        return RET_IO_ERROR;
    }

    if (utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD + payload_len]) != ota_protocol_calc_fcrc(buf, payload_len))
    {
        return RET_IO_ERROR;
    }

    /* --- Populate the output descriptor — payload pointer references the
     *     original buffer directly (zero-copy, no allocation) --- */
    frame->type = buf[OTA_OFFSET_TYPE];
    frame->flags = buf[OTA_OFFSET_FLAGS];
    frame->seq = utils_get_u16_le(&buf[OTA_OFFSET_SEQ]);
    frame->ack_seq = utils_get_u16_le(&buf[OTA_OFFSET_ACK_SEQ]);
    frame->cmd = utils_get_u16_le(&buf[OTA_OFFSET_CMD]);
    frame->payload_len = payload_len;
    frame->session_id = utils_get_u32_le(&buf[OTA_OFFSET_SESSION]);
    frame->payload = (payload_len == 0U) ? NULL : &buf[OTA_OFFSET_PAYLOAD];

    return RET_OK;
}

/**
 * @see ota_protocol.h for the full contract.
 *
 * Implementation notes:
 * - Fields are written in wire-order from SOF0 through the trailing FC.
 * - The HC is computed and written before the payload so that the FC coverage
 *   includes the finalized HC bytes in the header region.
 * - If a non-zero payload length is specified but the payload pointer is NULL,
 *   the function returns RET_INVALID_PARAM to catch programming errors early.
 */
int ota_protocol_build_frame(const ota_frame_t *frame, uint8_t *out, uint32_t out_size)
{
    uint32_t total_len;

    /* --- Parameter validation --- */
    if ((frame == NULL) || (out == NULL) ||
        ((frame->payload_len > 0U) && (frame->payload == NULL)))
    {
        return RET_INVALID_PARAM;
    }

    if (frame->payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* --- Compute total frame size and verify the output buffer is large enough --- */
    total_len = OTA_HEADER_LEN + (uint32_t)frame->payload_len + OTA_FRAME_CRC_LEN;
    if (out_size < total_len)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* --- Write fixed header fields in wire order --- */
    out[OTA_OFFSET_SOF0] = OTA_SOF0;
    out[OTA_OFFSET_SOF1] = OTA_SOF1;
    out[OTA_OFFSET_VER] = OTA_PROTO_VER;
    out[OTA_OFFSET_TYPE] = frame->type;
    out[OTA_OFFSET_FLAGS] = frame->flags;
    out[OTA_OFFSET_HLEN] = OTA_HEADER_LEN;
    utils_put_u16_le(&out[OTA_OFFSET_SEQ], frame->seq);
    utils_put_u16_le(&out[OTA_OFFSET_ACK_SEQ], frame->ack_seq);
    utils_put_u16_le(&out[OTA_OFFSET_CMD], frame->cmd);
    utils_put_u16_le(&out[OTA_OFFSET_PAYLOAD_LEN], frame->payload_len);
    utils_put_u32_le(&out[OTA_OFFSET_SESSION], frame->session_id);

    /* --- Compute and write the header CRC (HC) --- */
    utils_put_u16_le(&out[OTA_OFFSET_HCRC], ota_protocol_calc_hcrc(out));

    /* --- Copy payload into the frame body (if present) --- */
    if (frame->payload_len > 0U)
    {
        memcpy(&out[OTA_OFFSET_PAYLOAD], frame->payload, frame->payload_len);
    }

    /* --- Compute and write the frame CRC (FC) at the tail --- */
    utils_put_u16_le(&out[OTA_OFFSET_PAYLOAD + frame->payload_len], ota_protocol_calc_fcrc(out, frame->payload_len));

    /* Return the total wire-size of the constructed frame */
    return (int)total_len;
}
