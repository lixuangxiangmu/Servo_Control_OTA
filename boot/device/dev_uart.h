#ifndef DEV_UART_H
#define DEV_UART_H

#include <stdint.h>

#define DEV_UART_FRAME_MAX_LEN             512U

typedef enum
{
    UART_DATA_BITS_8 = 0,
    UART_DATA_BITS_9,
} uart_data_bits_t;

typedef enum
{
    UART_STOP_BITS_1 = 0,
    UART_STOP_BITS_2,
} uart_stop_bits_t;

typedef enum
{
    UART_PARITY_NONE = 0,
    UART_PARITY_EVEN,
    UART_PARITY_ODD,
} uart_parity_t;

typedef struct
{
    uint32_t baudrate;
    uart_data_bits_t data_bits;
    uart_stop_bits_t stop_bits;
    uart_parity_t parity;
} uart_config_t;

typedef struct
{
    uint8_t *buf;
    uint32_t len;
} uart_rx_request_t;

typedef struct
{
    const uint8_t *buf;
    uint32_t len;
} uart_tx_request_t;

typedef struct
{
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t rx_dropped_frames;
    uint32_t rx_overflow_frames;
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t tx_timeout_count;
} uart_stats_t;

typedef void (*uart_rx_isr_cb_t)(const char *name, const uint8_t *data, uint32_t len, void *user_ctx);
typedef void (*uart_tx_done_isr_cb_t)(const char *name, void *user_ctx);

typedef struct
{
    uart_rx_isr_cb_t rx_cb;
    uart_tx_done_isr_cb_t tx_done_cb;
    void *user_ctx;
} uart_callback_t;

#define DEV_UART_CMD_CONFIG        1
#define DEV_UART_CMD_ATTACH_CB     2
#define DEV_UART_CMD_RESTART_RX    3

/*****************************************************************************
@brief: 配置指定 UART 设备的串口参数
@para:name UART 逻辑设备名称
@para:cfg UART 配置参数指针
@return: RET_OK 表示配置成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_config(const char *name, const uart_config_t *cfg);

/*****************************************************************************
@brief: 从指定 UART 设备读取一条完整接收消息
@para:name UART 逻辑设备名称
@para:buf 接收数据输出缓冲区，缓冲区长度需不小于 DEV_UART_FRAME_MAX_LEN
@return: 正数表示实际读取字节数，负数表示失败原因
*******************************************************************************/
int dev_uart_read(const char *name, uint8_t *buf);

/*****************************************************************************
@brief: 通过指定 UART 设备发送一帧数据
@para:name UART 逻辑设备名称
@para:buf 待发送数据缓冲区
@para:len 待发送数据长度
@return: 正数表示已提交发送的字节数，负数表示失败原因
*******************************************************************************/
int dev_uart_write(const char *name, const uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: 清空指定 UART 设备的软件接收缓存并重启底层 DMA 接收
@para:name UART 逻辑设备名称
@return: RET_OK 表示清空成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_flush_rx(const char *name);

/*****************************************************************************
@brief: 获取指定 UART 设备的收发统计信息
@para:name UART 逻辑设备名称
@para:stats 统计信息输出指针
@return: RET_OK 表示获取成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_get_stats(const char *name, uart_stats_t *stats);

#endif
