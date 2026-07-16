/*****************************************************************************
 * @file    board_config.h
 * @brief   Bootloader 板级设备名称宏定义
 *
 * 仅保留 Bootloader 必需的外设:
 *   - UART3: 蓝牙 SPP 透传 (TX: PC10, RX: PC11)
 *   - I2C0:  EEPROM (SCL: PB6, SDA: PB7)
 *****************************************************************************/

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/* UART3 设备名称，用于微信小程序透传通信。 */
#define BOARD_UART3_NAME                    "uart3"

/* I2C0 设备名称，用于访问板载 EEPROM (AT24Cxx)。 */
#define BOARD_I2C0_NAME                     "i2c0"

/* On-chip Flash device name, used by boot OTA erase/write/read logic. */
#define BOARD_FLASH_NAME                    "flash0"

#endif /* BOARD_CONFIG_H */
