/*****************************************************************************
 * @file    boot_config.h
 * @brief   Bootloader 全局常量定义
 *
 * 汇总 Flash 分区、通信参数、超时策略、EEPROM 布局等所有常量。
 * 与 doc/OTA升级方案.md 保持严格一致。
 *****************************************************************************/

#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include "version_config.h"

#include <stdint.h>

/* =====================================================================
 * 协议常量
 * ===================================================================== */

/* 协议版本号。 */
#define BOOT_PROTOCOL_VER               0x01U

/* 帧定界符。 */
#define BOOT_FRAME_SOF0                 0x55U
#define BOOT_FRAME_SOF1                 0xAAU
#define BOOT_FRAME_EOF                  0x16U

/* 帧结构偏移量 (相对于 SOF 开始)。 */
#define BOOT_FRAME_SOF_OFFSET           0U
#define BOOT_FRAME_VER_OFFSET           2U
#define BOOT_FRAME_TYPE_OFFSET          3U
#define BOOT_FRAME_SEQ_OFFSET           4U
#define BOOT_FRAME_LEN_OFFSET           6U
#define BOOT_FRAME_PAYLOAD_OFFSET       8U

/* 帧长度约束。 */
#define BOOT_FRAME_MAX_PAYLOAD_LEN      500U
#define BOOT_FRAME_MAX_TOTAL_LEN        511U    /* SOF(2)+VER+TYPE+SEQ(2)+LEN(2)+PAYLOAD(500)+CRC16(2)+EOF = 511 */
#define BOOT_FRAME_HEADER_LEN           8U      /* SOF+VER+TYPE+SEQ+LEN */
#define BOOT_FRAME_TRAILER_LEN          3U      /* CRC16+EOF */

/* 推荐传输块大小 (平衡 BLE MTU 和 Flash 写入效率)。 */
#define BOOT_DEFAULT_BLOCK_SIZE         256U
#define BOOT_MIN_BLOCK_SIZE             16U

/* =====================================================================
 * 超时策略 (来自 OTA 方案 3.4 节)
 * ===================================================================== */

/* 上电等待上位机 HELLO 的超时时间 (ms)。 */
#define BOOT_STARTUP_TIMEOUT_MS         3000U

/* 升级过程中帧间超时 (ms)。 */
#define BOOT_FRAME_TIMEOUT_MS           500U

/* 升级态长超时 (ms) — 30 秒无通信后复位。 */
#define BOOT_UPGRADE_IDLE_TIMEOUT_MS    30000U

/* END 后等待 RESET 确认超时 (ms)。 */
#define BOOT_END_RESET_TIMEOUT_MS       10000U

/* NACK/超时累计错误次数上限，超过后进入长等待。 */
#define BOOT_MAX_CONSECUTIVE_ERRORS     5U

/* =====================================================================
 * 看门狗配置
 * ===================================================================== */

/* 独立看门狗溢出时间 (ms)，约 2 秒。 */
#define BOOT_IWDG_TIMEOUT_MS            2000U

/* OTA 元数据结构体大小 (固定 64 字节)。 */
#define BOOT_OTA_METADATA_SIZE          64U

/* EEPROM 设备名称 (需与 BSP 注册名称一致)。 */
#define BOOT_EEPROM_DEVICE_NAME         "eeprom0"

/* =====================================================================
 * OTA 元数据魔数
 * ===================================================================== */

/* 魔术字 "OTAG" 的 ASCII 编码，用于校验记录有效性。 */
#define BOOT_OTA_MAGIC                  0x4F544147UL

/* =====================================================================
 * 固件头校验 (来自 OTA 方案 11.2 节)
 * ===================================================================== */

/* 固件头魔术字 "WJWF"。 */
#define BOOT_FW_HEADER_MAGIC            0x46574A57UL

/* 固件头大小 (bytes)。 */
#define BOOT_FW_HEADER_SIZE             64U

/*
 * 固件头在 APP 镜像内的偏移量。
 * 必须放在 Cortex-M3 向量表之后（向量表约 336 字节），取 0x200 留足余量。
 * 上位机打包 bin 时必须将固件头放置在此偏移处。
 */
#define BOOT_FW_HEADER_OFFSET           0x200U

/* 固件头格式版本号。 */
#define BOOT_FW_HEADER_VERSION          0x0001U

/*
 * 固件头硬件 ID，标识目标产品型号。
 * 当前项目: 舵机控制 JW V2.0 = 0x01。
 * Bootloader 在校验固件头时要求 hw_id 与此值一致，防止误烧其他产品固件。
 */
#define BOOT_FW_HW_ID                   0x01U

/*
 * OTA protocol identity.
 * Product ID follows the manifest example in the OTA design document.
 */
#define BOOT_PRODUCT_ID                 0x00001001UL

/* =====================================================================
 * 升级重试限制
 * ===================================================================== */

/* 单次升级最大尝试次数。 */
#define BOOT_MAX_ATTEMPT_COUNT          5U

/* =====================================================================
 * 芯片信息 (用于 HELLO_ACK 响应)
 * ===================================================================== */

#define BOOT_CHIP_MODEL_STR             "GD32F103"

/* =====================================================================
 * 辅助宏
 * ===================================================================== */

/* 向上取整除法。 */
#define BOOT_DIV_ROUND_UP(a, b)         (((a) + (b) - 1U) / (b))

/* 取较小值。 */
#define BOOT_MIN(a, b)                  (((a) < (b)) ? (a) : (b))

/* 取较大值。 */
#define BOOT_MAX(a, b)                  (((a) > (b)) ? (a) : (b))

#endif /* BOOT_CONFIG_H */
