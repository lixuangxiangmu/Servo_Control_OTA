#include "dev_uart.h"

#include "device.h"
#include "return_code.h"

#include "FreeRTOS.h"
#include "message_buffer.h"
#include "semphr.h"

#define DEV_UART_DEVICE_MAX             4U
#define DEV_UART_RX_MSG_STORAGE_SIZE    (DEV_UART_FRAME_MAX_LEN + sizeof(size_t) + 1U)

typedef struct
{
    device_t *dev;
    uint8_t initialized;
    uint32_t pending_tx_len;
    MessageBufferHandle_t rx_msg;
    SemaphoreHandle_t tx_done_sem;
    uart_stats_t stats;
} dev_uart_runtime_t;

static dev_uart_runtime_t s_uart_runtime[DEV_UART_DEVICE_MAX];

/*****************************************************************************
@brief: 将 UART 抽象层超时参数转换为 FreeRTOS tick 等待时间
@para:timeout_ticks UART 接口传入的等待 tick，DEV_UART_WAIT_FOREVER 表示永久等待
@return: FreeRTOS 可直接使用的 TickType_t 等待时间
*******************************************************************************/
static TickType_t dev_uart_timeout_to_ticks(uint32_t timeout_ticks)
{
    return (timeout_ticks == DEV_UART_WAIT_FOREVER) ? portMAX_DELAY : (TickType_t)timeout_ticks;
}

/*****************************************************************************
@brief: 按名称查找 UART 设备并校验设备类型
@para:name UART 逻辑设备名称
@return: 找到并且类型匹配时返回 device_t 指针，否则返回空指针
*******************************************************************************/
static device_t *dev_uart_find(const char *name)
{
    device_t *dev;

    if ((name == 0) || (name[0] == '\0'))
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_UART))
    {
        return 0;
    }

    return dev;
}

/*****************************************************************************
@brief: 检查 UART 基础配置是否合法
@para:cfg UART 配置参数指针
@return: 合法返回 1，不合法返回 0
*******************************************************************************/
static uint8_t dev_uart_config_is_valid(const uart_config_t *cfg)
{
    if ((cfg == 0) || (cfg->baudrate == 0U) ||
        (cfg->data_bits > UART_DATA_BITS_9) ||
        (cfg->stop_bits > UART_STOP_BITS_2) ||
        (cfg->parity > UART_PARITY_ODD))
    {
        return 0U;
    }

    return 1U;
}

/*****************************************************************************
@brief: 根据 device 对象查找 UART 抽象层运行时上下文
@para:dev UART 设备对象指针
@return: 已绑定的运行时上下文，未找到时返回空指针
*******************************************************************************/
static dev_uart_runtime_t *dev_uart_find_runtime(device_t *dev)
{
    uint8_t i;

    for (i = 0U; i < DEV_UART_DEVICE_MAX; i++)
    {
        if (s_uart_runtime[i].dev == dev)
        {
            return &s_uart_runtime[i];
        }
    }

    return 0;
}

/*****************************************************************************
@brief: 为 UART 设备分配或复用一个抽象层运行时上下文
@para:dev UART 设备对象指针
@return: 成功返回运行时上下文指针，资源不足时返回空指针
*******************************************************************************/
static dev_uart_runtime_t *dev_uart_alloc_runtime(device_t *dev)
{
    dev_uart_runtime_t *runtime;
    uint8_t i;

    runtime = dev_uart_find_runtime(dev);
    if (runtime != 0)
    {
        return runtime;
    }

    for (i = 0U; i < DEV_UART_DEVICE_MAX; i++)
    {
        if (s_uart_runtime[i].dev == 0)
        {
            s_uart_runtime[i].dev = dev;
            return &s_uart_runtime[i];
        }
    }

    return 0;
}

/*****************************************************************************
@brief: UART 接收完成 ISR 回调，将一帧数据投递到 MessageBuffer
@para:name UART 逻辑设备名称
@para:data DMA 接收缓冲区数据指针
@para:len 本帧数据长度
@para:user_ctx dev_uart_runtime_t 运行时上下文
@return:
@note: 该函数运行在 UART IDLE 中断上下文中，只能调用 FromISR API。
*******************************************************************************/
/*****************************************************************************
@brief: 释放 UART 抽象层动态创建的 FreeRTOS 资源
@para:runtime UART 抽象层运行时上下文
@return:
*******************************************************************************/
static void dev_uart_release_runtime_resources(dev_uart_runtime_t *runtime)
{
    if (runtime == 0)
    {
        return;
    }

    if (runtime->tx_done_sem != 0)
    {
        vSemaphoreDelete(runtime->tx_done_sem);
        runtime->tx_done_sem = 0;
    }

    if (runtime->rx_msg != 0)
    {
        vMessageBufferDelete(runtime->rx_msg);
        runtime->rx_msg = 0;
    }
}

static void dev_uart_rx_isr_cb(const char *name, const uint8_t *data, uint32_t len, void *user_ctx)
{
    dev_uart_runtime_t *runtime = (dev_uart_runtime_t *)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;
    size_t sent_len;

    (void)name;

    if ((runtime == 0) || (runtime->rx_msg == 0) || (data == 0) || (len == 0U))
    {
        return;
    }

    if (len > DEV_UART_FRAME_MAX_LEN)
    {
        runtime->stats.rx_overflow_frames++;
        return;
    }

    sent_len = xMessageBufferSendFromISR(runtime->rx_msg, data, (size_t)len, &higher_priority_task_woken);
    if (sent_len == (size_t)len)
    {
        runtime->stats.rx_frames++;
        runtime->stats.rx_bytes += len;
    }
    else
    {
        runtime->stats.rx_dropped_frames++;
    }

    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/*****************************************************************************
@brief: UART 发送 DMA 完成 ISR 回调，释放发送完成信号量并更新统计
@para:name UART 逻辑设备名称
@para:user_ctx dev_uart_runtime_t 运行时上下文
@return:
@note: 该函数运行在 DMA 中断上下文中，只能调用 FromISR API。
*******************************************************************************/
static void dev_uart_tx_done_isr_cb(const char *name, void *user_ctx)
{
    dev_uart_runtime_t *runtime = (dev_uart_runtime_t *)user_ctx;
    BaseType_t higher_priority_task_woken = pdFALSE;

    (void)name;

    if ((runtime == 0) || (runtime->tx_done_sem == 0))
    {
        return;
    }

    runtime->stats.tx_frames++;
    runtime->stats.tx_bytes += runtime->pending_tx_len;
    runtime->pending_tx_len = 0U;

    (void)xSemaphoreGiveFromISR(runtime->tx_done_sem, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/*****************************************************************************
@brief: 准备 UART 抽象层运行环境并向底层驱动绑定 ISR 回调
@para:dev UART 设备对象指针
@para:runtime_out 输出 UART 抽象层运行时上下文
@return: RET_OK 表示准备完成，其他返回值表示失败原因
*******************************************************************************/
static int dev_uart_prepare(device_t *dev, dev_uart_runtime_t **runtime_out)
{
    dev_uart_runtime_t *runtime;
    uart_callback_t callbacks;
    int ret;

    if ((dev == 0) || (runtime_out == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        ret = device_init(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    runtime = dev_uart_alloc_runtime(dev);
    if (runtime == 0)
    {
        return RET_NO_RESOURCE;
    }

    if (runtime->initialized == 0U)
    {
        runtime->rx_msg = xMessageBufferCreate((size_t)DEV_UART_RX_MSG_STORAGE_SIZE);
        runtime->tx_done_sem = xSemaphoreCreateBinary();
        if ((runtime->rx_msg == 0) || (runtime->tx_done_sem == 0))
        {
            dev_uart_release_runtime_resources(runtime);
            return RET_NO_RESOURCE;
        }

        (void)xSemaphoreGive(runtime->tx_done_sem);

        callbacks.rx_cb = dev_uart_rx_isr_cb;
        callbacks.tx_done_cb = dev_uart_tx_done_isr_cb;
        callbacks.user_ctx = runtime;
        ret = device_control(dev, DEV_UART_CMD_ATTACH_CB, &callbacks);
        if (RET_IS_ERR(ret))
        {
            dev_uart_release_runtime_resources(runtime);
            return ret;
        }

        runtime->initialized = 1U;
    }

    *runtime_out = runtime;

    return RET_OK;
}

/*****************************************************************************
@brief: 配置指定 UART 设备的串口参数
@para:name UART 逻辑设备名称
@para:cfg UART 配置参数指针
@return: RET_OK 表示配置成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_config(const char *name, const uart_config_t *cfg)
{
    device_t *dev;
    int ret;

    if (dev_uart_config_is_valid(cfg) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_UART_CMD_CONFIG, (void *)cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 从指定 UART 设备读取一条完整接收消息
@para:name UART 逻辑设备名称
@para:buf 接收数据输出缓冲区，缓冲区长度需不小于 DEV_UART_FRAME_MAX_LEN
@para:timeout_ticks 等待消息的超时时间，单位为 FreeRTOS tick
@return: 正数表示实际读取字节数，负数表示失败原因
*******************************************************************************/
int dev_uart_read(const char *name, uint8_t *buf, uint32_t timeout_ticks)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    size_t next_len;
    size_t received;
    int ret;

    if (buf == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    received = xMessageBufferReceive(runtime->rx_msg, buf, (size_t)DEV_UART_FRAME_MAX_LEN, dev_uart_timeout_to_ticks(timeout_ticks));
    if (received == 0U)
    {
        next_len = xMessageBufferNextLengthBytes(runtime->rx_msg);
        return (next_len > (size_t)DEV_UART_FRAME_MAX_LEN) ? RET_BUFFER_TOO_SMALL : RET_TIMEOUT;
    }

    return RET_SUCCESS_VALUE(received);
}

/*****************************************************************************
@brief: 通过指定 UART 设备发送一帧数据
@para:name UART 逻辑设备名称
@para:buf 待发送数据缓冲区
@para:len 待发送数据长度
@para:timeout_ticks 等待发送资源的超时时间，单位为 FreeRTOS tick
@return: 正数表示已提交发送的字节数，负数表示失败原因
*******************************************************************************/
int dev_uart_write(const char *name, const uint8_t *buf, uint32_t len, uint32_t timeout_ticks)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    TickType_t wait_ticks;
    int ret;

    if ((buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (len > DEV_UART_FRAME_MAX_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    wait_ticks = dev_uart_timeout_to_ticks(timeout_ticks);

    if (xSemaphoreTake(runtime->tx_done_sem, wait_ticks) != pdTRUE)
    {
        runtime->stats.tx_timeout_count++;
        return RET_TIMEOUT;
    }

    runtime->pending_tx_len = len;
    ret = device_write(dev, buf, len);
    if (RET_IS_ERR(ret))
    {
        runtime->pending_tx_len = 0U;
        (void)xSemaphoreGive(runtime->tx_done_sem);
        return ret;
    }
    return ret;
}

/*****************************************************************************
@brief: 清空指定 UART 设备的软件接收缓存并重启底层 DMA 接收
@para:name UART 逻辑设备名称
@return: RET_OK 表示清空成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_flush_rx(const char *name)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    int ret;

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    (void)xMessageBufferReset(runtime->rx_msg);
    ret = device_control(dev, DEV_UART_CMD_RESTART_RX, 0);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 获取指定 UART 设备的收发统计信息
@para:name UART 逻辑设备名称
@para:stats 统计信息输出指针
@return: RET_OK 表示获取成功，其他返回值表示失败原因
*******************************************************************************/
int dev_uart_get_stats(const char *name, uart_stats_t *stats)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    int ret;

    if (stats == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    *stats = runtime->stats;

    return RET_OK;
}
