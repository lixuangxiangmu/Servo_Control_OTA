#include "ota_protocol.h"

#include "return_code.h"
#include "utils_lib.h"

#include <stddef.h>
#include <string.h>

#define OTA_OFFSET_SOF0              0U
#define OTA_OFFSET_SOF1              1U
#define OTA_OFFSET_VER               2U
#define OTA_OFFSET_TYPE              3U
#define OTA_OFFSET_FLAGS             4U
#define OTA_OFFSET_HLEN              5U
#define OTA_OFFSET_SEQ               6U
#define OTA_OFFSET_ACK_SEQ           8U
#define OTA_OFFSET_CMD               10U
#define OTA_OFFSET_PAYLOAD_LEN       12U
#define OTA_OFFSET_SESSION           14U
#define OTA_OFFSET_HCRC              18U
#define OTA_OFFSET_PAYLOAD           20U

#define OTA_HCRC_CALC_OFFSET         OTA_OFFSET_VER
#define OTA_HCRC_CALC_LEN            (OTA_OFFSET_HCRC - OTA_OFFSET_VER)
#define OTA_FCRC_CALC_OFFSET         OTA_OFFSET_VER
#define OTA_FCRC_BASE_LEN            (OTA_HEADER_LEN - OTA_OFFSET_VER)

static uint16_t ota_protocol_calc_hcrc(const uint8_t *buf)
{
    return utils_calculate_crc16(&buf[OTA_HCRC_CALC_OFFSET], OTA_HCRC_CALC_LEN);
}

static uint16_t ota_protocol_calc_fcrc(const uint8_t *buf, uint16_t payload_len)
{
    return utils_calculate_crc16(&buf[OTA_FCRC_CALC_OFFSET], (uint16_t)(OTA_FCRC_BASE_LEN + payload_len));
}

int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame)
{
    uint16_t payload_len;
    uint32_t total_len;

    if ((buf == NULL) || (frame == NULL))
    {
        return RET_INVALID_PARAM;
    }

    if (len < OTA_FRAME_MIN_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    if ((buf[OTA_OFFSET_SOF0] != OTA_SOF0) ||
        (buf[OTA_OFFSET_SOF1] != OTA_SOF1) ||
        (buf[OTA_OFFSET_VER] != OTA_PROTO_VER) ||
        (buf[OTA_OFFSET_HLEN] != OTA_HEADER_LEN))
    {
        return RET_FAIL;
    }

    payload_len = utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD_LEN]);
    if (payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    total_len = OTA_HEADER_LEN + (uint32_t)payload_len + OTA_FRAME_CRC_LEN;
    if (len != total_len)
    {
        return RET_INVALID_PARAM;
    }

    if (utils_get_u16_le(&buf[OTA_OFFSET_HCRC]) != ota_protocol_calc_hcrc(buf))
    {
        return RET_IO_ERROR;
    }

    if (utils_get_u16_le(&buf[OTA_OFFSET_PAYLOAD + payload_len]) != ota_protocol_calc_fcrc(buf, payload_len))
    {
        return RET_IO_ERROR;
    }

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

int ota_protocol_build_frame(const ota_frame_t *frame, uint8_t *out, uint32_t out_size)
{
    uint32_t total_len;

    if ((frame == NULL) || (out == NULL) ||
        ((frame->payload_len > 0U) && (frame->payload == NULL)))
    {
        return RET_INVALID_PARAM;
    }

    if (frame->payload_len > OTA_MAX_PAYLOAD)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    total_len = OTA_HEADER_LEN + (uint32_t)frame->payload_len + OTA_FRAME_CRC_LEN;
    if (out_size < total_len)
    {
        return RET_BUFFER_TOO_SMALL;
    }

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
    utils_put_u16_le(&out[OTA_OFFSET_HCRC], ota_protocol_calc_hcrc(out));

    if (frame->payload_len > 0U)
    {
        memcpy(&out[OTA_OFFSET_PAYLOAD], frame->payload, frame->payload_len);
    }

    utils_put_u16_le(&out[OTA_OFFSET_PAYLOAD + frame->payload_len], ota_protocol_calc_fcrc(out, frame->payload_len));

    return (int)total_len;
}
