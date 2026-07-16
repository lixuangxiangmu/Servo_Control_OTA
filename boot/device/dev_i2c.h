#ifndef DEV_I2C_H
#define DEV_I2C_H

#include <stdint.h>

#define DEV_I2C_WAIT_FOREVER            0xFFFFFFFFU    /* 永久等待标志 */
#define DEV_I2C_DEFAULT_TIMEOUT_MS      100U           /* I2C 默认超时时间，单位：ms */

typedef enum
{
    I2C_ADDR_7BIT = 0,     /* 7 位从机地址模式 */
    I2C_ADDR_10BIT,        /* 10 位从机地址模式 */
} i2c_addr_mode_t;

/* I2C 总线运行配置。 */
typedef struct
{
    uint32_t speed_hz;             /* I2C 总线速率，单位：Hz */
    i2c_addr_mode_t addr_mode;     /* 地址模式 */
    uint16_t own_addr;             /* 主机自身地址，硬件初始化时使用 */
} i2c_config_t;

/* 单次 I2C 消息描述，可表示写、读或先写寄存器地址再读。 */
typedef struct
{
    uint8_t slave_addr;            /* 7 位 I2C 从机地址 */
    uint16_t reg_addr;             /* 从机内部寄存器/存储器地址 */
    uint8_t reg_addr_width;        /* 寄存器地址宽度：0、1 或 2 字节 */
    const uint8_t *tx_buf;         /* 发送数据缓冲区 */
    uint8_t *rx_buf;               /* 接收数据缓冲区 */
    uint32_t tx_len;               /* 发送数据长度，单位：字节 */
    uint32_t rx_len;               /* 接收数据长度，单位：字节 */
    uint32_t timeout_ms;           /* 本消息超时时间，0 表示使用默认超时 */
} i2c_msg_t;

/* 多消息传输请求。 */
typedef struct
{
    i2c_msg_t *msgs;       /* I2C 消息数组 */
    uint32_t num;          /* 消息数量 */
} i2c_transfer_t;

/* I2C 总线扫描请求。 */
typedef struct
{
    uint8_t *addr_buf;         /* 扫描到的从机地址输出缓冲区 */
    uint32_t max_num;          /* addr_buf 最多可保存的地址数量 */
    uint32_t *found_num;       /* 实际发现的从机数量 */
    uint32_t timeout_ms;       /* 单地址探测超时时间，0 表示使用默认超时 */
} i2c_scan_t;

/* I2C 单地址探测请求。 */
typedef struct
{
    uint8_t slave_addr;        /* 待探测的 7 位 I2C 从机地址 */
    uint32_t timeout_ms;       /* 探测超时时间，0 表示使用默认超时 */
} i2c_probe_t;

#define DEV_I2C_CMD_CONFIG              1    /* 配置 I2C 总线 */
#define DEV_I2C_CMD_TRANSFER            2    /* 执行 I2C 消息传输 */
#define DEV_I2C_CMD_SCAN                3    /* 扫描总线上的从机地址 */
#define DEV_I2C_CMD_BUS_RECOVER         4    /* 尝试恢复被拉低的 I2C 总线 */
#define DEV_I2C_CMD_PROBE               5    /* 探测指定从机地址是否应答 */

/*****************************************************************************
@brief: 配置指定 I2C 设备
@para:name I2C 设备名称
@para:cfg I2C 总线配置指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_config(const char *name, const i2c_config_t *cfg);

/*****************************************************************************
@brief: 执行一组 I2C 消息传输
@para:name I2C 设备名称
@para:msgs I2C 消息数组
@para:num 消息数量
@return: RET_OK 表示传输成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_transfer(const char *name, i2c_msg_t *msgs, uint32_t num);

/*****************************************************************************
@brief: 使用 1 字节寄存器地址写入 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:data 待写入数据缓冲区
@para:len 写入长度，单位：字节
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_write(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                  const uint8_t *data, uint32_t len);

/*****************************************************************************
@brief: 使用 1 字节寄存器地址读取 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:data 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_read(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                 uint8_t *data, uint32_t len);

/*****************************************************************************
@brief: 按指定寄存器地址宽度写入 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:reg_addr_width 寄存器地址宽度：0、1 或 2 字节
@para:data 待写入数据缓冲区
@para:len 写入长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_mem_write(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                      uint8_t reg_addr_width, const uint8_t *data, uint32_t len,
                      uint32_t timeout_ms);

/*****************************************************************************
@brief: 按指定寄存器地址宽度读取 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:reg_addr_width 寄存器地址宽度：0、1 或 2 字节
@para:data 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_mem_read(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                     uint8_t reg_addr_width, uint8_t *data, uint32_t len,
                     uint32_t timeout_ms);

/*****************************************************************************
@brief: 扫描 I2C 总线上应答的从机地址
@para:name I2C 设备名称
@para:addr_buf 地址输出缓冲区
@para:max_num addr_buf 最多可保存的地址数量
@para:found_num 实际发现的从机数量输出指针
@return: RET_OK 表示扫描成功，RET_BUFFER_TOO_SMALL 表示缓冲区不足，其他返回值表示失败
*******************************************************************************/
int dev_i2c_scan(const char *name, uint8_t *addr_buf, uint32_t max_num,
                 uint32_t *found_num);

/*****************************************************************************
@brief: 尝试恢复 I2C 总线
@para:name I2C 设备名称
@return: RET_OK 表示恢复成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_bus_recover(const char *name);

/*****************************************************************************
@brief: 探测指定 I2C 从机地址是否应答
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示从机应答，其他返回值表示未应答或失败
*******************************************************************************/
int dev_i2c_probe(const char *name, uint8_t slave_addr, uint32_t timeout_ms);

#endif
