#ifndef EEPROM_H
#define EEPROM_H

#include <stdint.h>

typedef enum
{
    EEPROM_TYPE_AT24C08 = 0,    /* AT24C08：1KB 容量，1 字节字地址，设备地址携带块选择位 */
    EEPROM_TYPE_AT24C32,        /* AT24C32：4KB 容量，2 字节字地址 */
    EEPROM_TYPE_CUSTOM,         /* 自定义 EEPROM 参数，由 custom_* 字段给出 */
} eeprom_type_t;

/* EEPROM 注册配置，用于描述一颗挂在指定 I2C 总线上的 EEPROM 器件。 */
typedef struct
{
    const char *name;                   /* EEPROM 设备名称，后续读写通过该名称查找设备 */
    const char *i2c_name;               /* 底层 I2C 设备名称 */
    eeprom_type_t type;                 /* EEPROM 型号或自定义类型 */
    uint8_t slave_addr;                 /* 7 位 I2C 从机地址；填 0 时使用默认地址 0x50 */
    uint32_t write_cycle_timeout_ms;    /* 写周期 ACK 轮询超时时间，单位 ms；填 0 时使用默认值 */
    uint32_t custom_total_size;         /* 自定义总容量，单位：字节，仅 CUSTOM 类型有效 */
    uint8_t custom_page_size;           /* 自定义页大小，单位：字节，仅 CUSTOM 类型有效 */
    uint8_t custom_addr_width;          /* 自定义字地址宽度：1 或 2 字节，仅 CUSTOM 类型有效 */
    uint8_t custom_block_bits;          /* 自定义块选择位数，位于从机地址低位，仅 CUSTOM 类型有效 */
} eeprom_cfg_t;

/* EEPROM 运行时信息，用于向上层查询注册后的实际参数。 */
typedef struct
{
    uint32_t total_size;    /* EEPROM 总容量，单位：字节 */
    uint8_t page_size;      /* 单页可连续写入的字节数 */
    uint8_t addr_width;     /* EEPROM 内部字地址宽度，单位：字节 */
    uint8_t block_bits;     /* 通过从机地址选择存储块的位数 */
    uint8_t slave_addr;     /* 基础 7 位 I2C 从机地址 */
} eeprom_info_t;

/*****************************************************************************
@brief: 注册一颗 EEPROM 设备
@para:cfg EEPROM 注册配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int eeprom_register(const eeprom_cfg_t *cfg);

/*****************************************************************************
@brief: 从指定 EEPROM 地址读取数据
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@return: 成功返回实际读取长度，失败返回对应错误码
*******************************************************************************/
int eeprom_read(const char *name, uint32_t addr, uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: 向指定 EEPROM 地址写入数据
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 待写入数据缓冲区
@para:len 写入长度，单位：字节
@return: 成功返回实际写入长度，失败返回对应错误码
*******************************************************************************/
int eeprom_write(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: 更新 EEPROM 数据，仅在内容变化时执行写入
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 目标数据缓冲区
@para:len 更新长度，单位：字节
@return: 成功返回实际更新长度，失败返回对应错误码
*******************************************************************************/
int eeprom_update(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: 获取已注册 EEPROM 的运行时信息
@para:name EEPROM 设备名称
@para:info EEPROM 信息输出指针
@return: RET_OK 表示获取成功，其他返回值表示失败
*******************************************************************************/
int eeprom_get_info(const char *name, eeprom_info_t *info);

#endif
