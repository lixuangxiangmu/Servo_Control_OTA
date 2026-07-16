#include "ota_protocol.h"

#include "return_code.h"
#include "utils_lib.h"

#include <stdlib.h>

/*===========================================================================
 * Wire-Format Byte Offsets
 *
 * These constants define the exact byte positions of each field within the
 * raw frame buffer. All multi-byte integers are stored in little-endian
 * byte order.
 *===========================================================================*/

/** Offset of SOF0 byte (0x55). */
#define OTA_OFFSET_SOF0              0U
/** Offset of SOF1 byte (0xAA). */
#define OTA_OFFSET_SOF1              1U
/** Offset of protocol version byte. */
#define OTA_OFFSET_VER               2U
/** Offset of frame type byte. */
#define OTA_OFFSET_TYPE              3U
/** Offset of flags byte. */
#define OTA_OFFSET_FLAGS             4U
/** Offset of header length byte (HLEN). */
#define OTA_OFFSET_HLEN              5U
/** Offset of sequence number field (2 bytes, LE). */
#define OTA_OFFSET_SEQ               6U
/** Offset of acknowledged sequence number field (2 bytes, LE). */
#define OTA_OFFSET_ACK_SEQ           8U
/** Offset of command ID field (2 bytes, LE). */
#define OTA_OFFSET_CMD               10U
/** Offset of payload length field (2 bytes, LE). */
#define OTA_OFFSET_PAYLOAD_LEN       12U
/** Offset of session ID field (4 bytes, LE). */
#define OTA_OFFSET_SESSION           14U
/** Offset of header CRC field (2 bytes, LE). */
#define OTA_OFFSET_HCRC              18U
/** Offset where payload data begins (immediately after the 20-byte header). */
#define OTA_OFFSET_PAYLOAD           20U

/* ---- CRC Calculation Ranges ---- */

/**
 * HCRC (Header CRC) calculation: starts at VER (offset 2) and covers
 * 16 bytes up to but not including the HCRC field itself.
 */
#define OTA_HCRC_CALC_OFFSET      OTA_OFFSET_VER
#define OTA_HCRC_CALC_LEN         (OTA_OFFSET_HCRC - OTA_OFFSET_VER)

/**
 * FCRC (Full-frame CRC) calculation: starts at VER (offset 2). The base
 * length is the header minus the 2 SOF bytes, and the payload length is
 * added at call time.
 */
#define OTA_FCRC_CALC_OFFSET      OTA_OFFSET_VER
#define OTA_FCRC_BASE_LEN         (OTA_HEADER_LEN - OTA_OFFSET_VER)


/*===========================================================================
 * Internal CRC Calculation Helpers
 *===========================================================================*/

/**
 * @brief Compute the 16-bit header CRC over the header fields.
 *
 * Calculates CRC-16-CCITT over the bytes from VER (offset 2) through SESSION_ID (offset 18, exclusive). 
 * The SOF bytes and HCRC field itself are excluded from this calculation.
 *
 * @param buf Pointer to the frame buffer (must point to SOF0, the beginning
 *            of the frame).
 * @return 16-bit CRC value for the header portion.
 */
static uint16_t ota_protocol_calc_hcrc(const uint8_t *buf)
{
    return utils_calculate_crc16(&buf[OTA_HCRC_CALC_OFFSET], OTA_HCRC_CALC_LEN);
}

/**
 * @brief Compute the 16-bit full-frame CRC over the header and payload.
 *
 * Calculates CRC-16-CCITT over the bytes from VER (offset 2) through the
 * end of the payload. The SOF bytes and both CRC fields are excluded.
 *
 * The calculation range is: [VER .. PAYLOAD_END], i.e. offsets [2, 20 +
 * payload_len). This ensures that any corruption in the header or payload
 * is detected.
 *
 * @param buf         Pointer to the frame buffer (must point to SOF0).
 * @param payload_len Length of the payload in bytes.
 * @return 16-bit CRC value for the entire frame (excluding SOF and CRCs).
 */
static uint16_t ota_protocol_calc_fcrc(const uint8_t *buf, uint16_t payload_len)
{
    return utils_calculate_crc16(&buf[OTA_FCRC_CALC_OFFSET], (uint16_t)(OTA_FCRC_BASE_LEN + payload_len));
}


/*===========================================================================
 * Frame Parser
 *===========================================================================*/

/**
 * @brief Parse and validate a raw byte buffer into an ota_frame_t structure.
 *
 * This function performs a thorough multi-stage validation before populating
 * the output frame descriptor:
 *
 * Stage 1 — Structural Validation:
 * Stage 2 — Payload Length Validation:
 * Stage 3 — CRC Integrity Checks:
 *
 * @param buf   Pointer to the raw received byte buffer.
 * @param len   Number of valid bytes in the buffer.
 * @param frame Pointer to an ota_frame_t to fill with parsed data.
 * @return RET_OK (0) on success, or a negative error code on failure.
 */
int ota_protocol_parse_header(const uint8_t *buf, uint32_t len, ota_frame_t *frame)
{
    uint16_t payload_len;
    uint32_t total_len;
    uint16_t hcrc_rx;
    uint16_t hcrc_calc;

    /* ---- Stage 1: NULL pointer check ---- */
    if ((buf == NULL) || (frame == NULL))
    {
        return RET_INVALID_PARAM;
    }

    /* ---- Stage 1: Minimum length — must be at least header + FCRC ---- */
    if (len < OTA_FRAME_MIN_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* ---- Stage 1: Magic number, protocol version, and header length ---- */
    if ((buf[OTA_OFFSET_SOF0] != OTA_SOF0) ||
        (buf[OTA_OFFSET_SOF1] != OTA_SOF1) ||
        (buf[OTA_OFFSET_VER] != OTA_PROTO_VER) ||
        (buf[OTA_OFFSET_HLEN] != OTA_HEADER_LEN))
    {
        return RET_FAIL;
    }

    /* ---- Stage 2: Extract and validate payload length ---- */
    payload_len = utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD_LEN]);
    if (payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* ---- Stage 2: Verify total length matches expected ---- */
    total_len = (uint32_t)OTA_HEADER_LEN + (uint32_t)payload_len + (uint32_t)OTA_FRAME_CRC_LEN;
    if (len != total_len)
    {
        return RET_INVALID_PARAM;
    }

    /* ---- Stage 3: Validate header CRC (HCRC) ---- */
    hcrc_rx = utils_get_u16_le(&buf[OTA_OFFSET_HCRC]);
    hcrc_calc = ota_protocol_calc_hcrc(buf);
    if (hcrc_rx != hcrc_calc)
    {
        return RET_IO_ERROR;
    }

    /* ---- Populate frame descriptor from validated buffer ---- */
    frame->type = buf[OTA_OFFSET_TYPE];
    frame->flags = buf[OTA_OFFSET_FLAGS];
    frame->seq = utils_get_u16_le(&buf[OTA_OFFSET_SEQ]);
    frame->ack_seq = utils_get_u16_le(&buf[OTA_OFFSET_ACK_SEQ]);
    frame->cmd = utils_get_u16_le(&buf[OTA_OFFSET_CMD]);
    frame->payload_len = payload_len;
    frame->session_id = utils_get_u32_le(&buf[OTA_OFFSET_SESSION]);

    /* Payload pointer is NULL for zero-length payloads; otherwise points
       directly into the original receive buffer at offset 20. */
    frame->payload = (payload_len == 0U) ? NULL : &buf[OTA_OFFSET_PAYLOAD];

    return RET_OK;
}

int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame)
{
    uint16_t fcrc_rx;
    uint16_t fcrc_calc;
    int ret;

    ret = ota_protocol_parse_header(buf, len, frame);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    fcrc_rx = utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD + frame->payload_len]);
    fcrc_calc = ota_protocol_calc_fcrc(buf, frame->payload_len);
    if (fcrc_rx != fcrc_calc)
    {
        return RET_IO_ERROR;
    }

    return RET_OK;
}


/*===========================================================================
 * Frame Builder
 *===========================================================================*/

/**
 * @brief Build a complete wire-format OTA frame.
 *
 * @param frame    Logical OTA frame descriptor.
 * @param out      Destination buffer.
 * @param out_size Destination buffer capacity.
 */
int ota_protocol_build_frame(const ota_frame_t *frame, uint8_t *out, uint32_t out_size)
{
    uint32_t total_len;
    uint16_t hcrc;
    uint16_t fcrc;

    /* ---- Parameter validation ---- */

    /* Output buffer must be non-NULL. */
    if ((frame == NULL) || (out == NULL))
    {
        return RET_INVALID_PARAM;
    }

    /* If a payload length is specified, the payload pointer must be valid. */
    if ((frame->payload_len > 0U) && (frame->payload == NULL))
    {
        return RET_INVALID_PARAM;
    }

    /* Payload length must not exceed the protocol maximum. */
    if (frame->payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* Verify the output buffer can hold the complete frame. */
    total_len = (uint32_t)OTA_HEADER_LEN + (uint32_t)frame->payload_len + (uint32_t)OTA_FRAME_CRC_LEN;
    if (out_size < total_len)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    /* ---- Write fixed single-byte header fields ---- */
    out[OTA_OFFSET_SOF0] = OTA_SOF0;          /* Start-of-Frame byte 0: 0x55 */
    out[OTA_OFFSET_SOF1] = OTA_SOF1;          /* Start-of-Frame byte 1: 0xAA */
    out[OTA_OFFSET_VER] = OTA_PROTO_VER;      /* Protocol version */
    out[OTA_OFFSET_TYPE] = frame->type;              /* Frame type */
    out[OTA_OFFSET_FLAGS] = frame->flags;            /* Capability flags */
    out[OTA_OFFSET_HLEN] = OTA_HEADER_LEN;    /* Fixed header length */

    /* ---- Write multi-byte header fields (little-endian) ---- */
    utils_put_u16_le(&out[OTA_OFFSET_SEQ], frame->seq);
    utils_put_u16_le(&out[OTA_OFFSET_ACK_SEQ], frame->ack_seq);
    utils_put_u16_le(&out[OTA_OFFSET_CMD], frame->cmd);
    utils_put_u16_le(&out[OTA_OFFSET_PAYLOAD_LEN], frame->payload_len);
    utils_put_u32_le(&out[OTA_OFFSET_SESSION], frame->session_id);

    /* ---- Compute and write header CRC (HCRC) ---- */
    hcrc = ota_protocol_calc_hcrc(out);
    utils_put_u16_le(&out[OTA_OFFSET_HCRC], hcrc);

    /* ---- Copy payload data (if any) ---- */
    if (frame->payload_len > 0U)
    {
        uint16_t i;

        for (i = 0U; i < frame->payload_len; i++)
        {
            out[OTA_OFFSET_PAYLOAD + i] = frame->payload[i];
        }
    }

    /* ---- Compute and write full-frame CRC (FCRC) ---- */
    /* FCRC is placed immediately after the last payload byte. */
    fcrc = ota_protocol_calc_fcrc(out, frame->payload_len);
    utils_put_u16_le(&out[OTA_OFFSET_PAYLOAD + frame->payload_len], fcrc);

    /* Return the total frame length so the caller can use it directly for
       transmission (e.g., as the count argument to a UART write). */
    return (int)total_len;
}
