#ifndef OTA_PROTOCOL_H
#define OTA_PROTOCOL_H

#include <stdint.h>

#define OTA_SOF0                    0x55U
#define OTA_SOF1                    0xAAU
#define OTA_PROTO_VER               0x01U
#define OTA_HEADER_LEN              20U
#define OTA_FRAME_CRC_LEN           2U
#define OTA_FRAME_MIN_LEN           (OTA_HEADER_LEN + OTA_FRAME_CRC_LEN)
#define OTA_MAX_PAYLOAD             490U
#define OTA_MAX_FRAME_LEN           (OTA_HEADER_LEN + OTA_MAX_PAYLOAD + OTA_FRAME_CRC_LEN)

#define OTA_SUPPORT_RESUME          (1UL << 0)

typedef enum
{
    FRAME_TYPE_REQ = 0x01U,
    FRAME_TYPE_RSP = 0x02U,
} ota_frame_type_t;

typedef enum
{
    CMD_GET_INFO  = 0x0002U,
    CMD_ENTER_OTA = 0x0004U,
} ota_cmd_t;

typedef enum
{
    OTA_OK                       = 0x0000U,
    OTA_ERR_UNKNOWN_CMD          = 0x0001U,
    OTA_ERR_PAYLOAD_LEN          = 0x0003U,
    OTA_ERR_PRODUCT_MISMATCH     = 0x0100U,
    OTA_ERR_HW_MISMATCH          = 0x0101U,
    OTA_ERR_VERSION_TOO_OLD      = 0x0102U,
    OTA_ERR_IMAGE_TOO_LARGE      = 0x0103U,
    OTA_ERR_EEPROM               = 0x0206U,
} ota_status_t;

typedef struct
{
    uint8_t type;
    uint8_t flags;
    uint16_t seq;
    uint16_t ack_seq;
    uint16_t cmd;
    uint16_t payload_len;
    uint32_t session_id;
    const uint8_t *payload;
} ota_frame_t;

/** Parse one complete OTA frame and validate both header and frame CRCs. */
int ota_protocol_parse_frame(const uint8_t *buf, uint32_t len, ota_frame_t *frame);

/** Build one complete OTA frame in the common little-endian wire format. */
int ota_protocol_build_frame(const ota_frame_t *frame,
                             uint8_t *out,
                             uint32_t out_size);

#endif /* OTA_PROTOCOL_H */
