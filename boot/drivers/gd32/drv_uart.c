#include "drv_uart.h"

#include "device.h"
#include "return_code.h"

#include <string.h>

#define GD32_UART_DEVICE_MAX        4U
#define GD32_UART_DATA_REG_OFFSET   0x04U

typedef struct
{
    uint32_t uart_periph;
    rcu_periph_enum uart_rcu;
    rcu_periph_enum gpio_rcu;
    uint32_t gpio_periph;
    uint32_t tx_pin;
    uint32_t rx_pin;
    IRQn_Type uart_irq;
    uint32_t dma_periph;
    rcu_periph_enum dma_rcu;
    dma_channel_enum dma_rx_channel;
    dma_channel_enum dma_tx_channel;
    IRQn_Type dma_tx_irq;
    uint8_t irq_preemption_priority;
    uint8_t irq_sub_priority;
    uart_config_t config;
    uart_callback_t callbacks;
    uint8_t rx_dma_buf[GD32_UART_FRAME_MAX_LEN];
    uint8_t tx_dma_buf[GD32_UART_FRAME_MAX_LEN];
    uint8_t tx_busy;
} gd32_uart_dev_t;

typedef struct
{
    device_t dev;
    gd32_uart_dev_t uart;
} gd32_uart_slot_t;

static gd32_uart_slot_t s_uart_slots[GD32_UART_DEVICE_MAX];
static uint8_t s_uart_count;

/*****************************************************************************
@brief: 检查 UART 基础配置是否合法
@para:cfg UART 配置参数指针
@return: 合法返回 1，不合法返回 0
*******************************************************************************/
static uint8_t gd32_uart_config_is_valid(const uart_config_t *cfg)
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
@brief: 获取 GD32 USART 数据寄存器地址
@para:uart_periph USART/UART 外设基地址
@return: USART/UART 数据寄存器地址
*******************************************************************************/
static uint32_t gd32_uart_data_reg_addr(uint32_t uart_periph)
{
    return uart_periph + GD32_UART_DATA_REG_OFFSET;
}

/*****************************************************************************
@brief: 根据 USART/UART 外设基地址查找 UART 注册槽
@para:uart_periph USART/UART 外设基地址
@return: 找到时返回注册槽指针，否则返回空指针
*******************************************************************************/
static gd32_uart_slot_t *gd32_uart_find_slot_by_periph(uint32_t uart_periph)
{
    uint8_t i;

    for (i = 0U; i < s_uart_count; i++)
    {
        if (s_uart_slots[i].uart.uart_periph == uart_periph)
        {
            return &s_uart_slots[i];
        }
    }

    return 0;
}

/*****************************************************************************
@brief: 将抽象层数据位配置转换为 GD32 标准库参数
@para:data_bits 抽象层 UART 数据位枚举
@return: GD32 USART_WL_xBIT 参数
*******************************************************************************/
static uint32_t gd32_uart_word_len_to_hw(uart_data_bits_t data_bits)
{
    return (data_bits == UART_DATA_BITS_9) ? USART_WL_9BIT : USART_WL_8BIT;
}

/*****************************************************************************
@brief: 将抽象层停止位配置转换为 GD32 标准库参数
@para:stop_bits 抽象层 UART 停止位枚举
@return: GD32 USART_STB_xBIT 参数
*******************************************************************************/
static uint32_t gd32_uart_stop_bits_to_hw(uart_stop_bits_t stop_bits)
{
    return (stop_bits == UART_STOP_BITS_2) ? USART_STB_2BIT : USART_STB_1BIT;
}

/*****************************************************************************
@brief: 将抽象层校验位配置转换为 GD32 标准库参数
@para:parity 抽象层 UART 校验位枚举
@return: GD32 USART_PM_xxx 参数
*******************************************************************************/
static uint32_t gd32_uart_parity_to_hw(uart_parity_t parity)
{
    if (parity == UART_PARITY_EVEN)
    {
        return USART_PM_EVEN;
    }

    if (parity == UART_PARITY_ODD)
    {
        return USART_PM_ODD;
    }

    return USART_PM_NONE;
}

/*****************************************************************************
@brief: 重启 UART RX DMA 接收
@para:uart GD32 UART 私有硬件上下文
@return:
*******************************************************************************/
static void gd32_uart_rx_dma_restart(gd32_uart_dev_t *uart)
{
    dma_channel_disable(uart->dma_periph, uart->dma_rx_channel);
    dma_transfer_number_config(uart->dma_periph, uart->dma_rx_channel, GD32_UART_FRAME_MAX_LEN);
    dma_channel_enable(uart->dma_periph, uart->dma_rx_channel);
}

/*****************************************************************************
@brief: 将当前 UART 配置写入 GD32 USART/UART 外设
@para:uart GD32 UART 私有硬件上下文
@return: RET_OK 表示配置成功，其他返回值表示失败原因
*******************************************************************************/
static int gd32_uart_apply_config(gd32_uart_dev_t *uart)
{
    if ((uart == 0) || (gd32_uart_config_is_valid(&uart->config) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    usart_disable(uart->uart_periph);
    usart_deinit(uart->uart_periph);
    usart_baudrate_set(uart->uart_periph, uart->config.baudrate);
    usart_word_length_set(uart->uart_periph, gd32_uart_word_len_to_hw(uart->config.data_bits));
    usart_stop_bit_set(uart->uart_periph, gd32_uart_stop_bits_to_hw(uart->config.stop_bits));
    usart_parity_config(uart->uart_periph, gd32_uart_parity_to_hw(uart->config.parity));
    usart_receive_config(uart->uart_periph, USART_RECEIVE_ENABLE);
    usart_transmit_config(uart->uart_periph, USART_TRANSMIT_ENABLE);
    usart_interrupt_enable(uart->uart_periph, USART_INT_IDLE);
    usart_dma_receive_config(uart->uart_periph, USART_RECEIVE_DMA_ENABLE);
    usart_dma_transmit_config(uart->uart_periph, USART_TRANSMIT_DMA_ENABLE);
    usart_enable(uart->uart_periph);

    return RET_OK;
}

/*****************************************************************************
@brief: 初始化 UART 使用的 RX/TX DMA 通道
@para:uart GD32 UART 私有硬件上下文
@return:
*******************************************************************************/
static void gd32_uart_dma_init(gd32_uart_dev_t *uart)
{
    dma_parameter_struct dma_init_struct;

    dma_deinit(uart->dma_periph, uart->dma_rx_channel);
    dma_init_struct.direction = DMA_PERIPHERAL_TO_MEMORY;
    dma_init_struct.periph_addr = gd32_uart_data_reg_addr(uart->uart_periph);
    dma_init_struct.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_init_struct.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    dma_init_struct.memory_addr = (uint32_t)uart->rx_dma_buf;
    dma_init_struct.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    dma_init_struct.memory_width = DMA_MEMORY_WIDTH_8BIT;
    dma_init_struct.number = GD32_UART_FRAME_MAX_LEN;
    dma_init_struct.priority = DMA_PRIORITY_HIGH;
    dma_init(uart->dma_periph, uart->dma_rx_channel, &dma_init_struct);
    dma_circulation_disable(uart->dma_periph, uart->dma_rx_channel);
    dma_channel_enable(uart->dma_periph, uart->dma_rx_channel);

    dma_deinit(uart->dma_periph, uart->dma_tx_channel);
    dma_init_struct.direction = DMA_MEMORY_TO_PERIPHERAL;
    dma_init_struct.periph_addr = gd32_uart_data_reg_addr(uart->uart_periph);
    dma_init_struct.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_init_struct.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
    dma_init_struct.memory_addr = (uint32_t)uart->tx_dma_buf;
    dma_init_struct.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    dma_init_struct.memory_width = DMA_MEMORY_WIDTH_8BIT;
    dma_init_struct.number = 0U;
    dma_init_struct.priority = DMA_PRIORITY_HIGH;
    dma_init(uart->dma_periph, uart->dma_tx_channel, &dma_init_struct);
    dma_circulation_disable(uart->dma_periph, uart->dma_tx_channel);
    dma_channel_disable(uart->dma_periph, uart->dma_tx_channel);
    dma_interrupt_enable(uart->dma_periph, uart->dma_tx_channel, DMA_INT_FTF);
}

/*****************************************************************************
@brief: 初始化 GD32 UART 硬件资源
@para:dev 设备层传入的 UART 设备对象
@return: RET_OK 表示初始化成功，其他返回值表示失败原因
*******************************************************************************/
static int gd32_uart_init(device_t *dev)
{
    gd32_uart_dev_t *uart;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    uart = (gd32_uart_dev_t *)dev->user_data;       //解析出串口私有数据

    rcu_periph_clock_enable(uart->gpio_rcu);
    rcu_periph_clock_enable(uart->uart_rcu);
    rcu_periph_clock_enable(uart->dma_rcu);

    gpio_init(uart->gpio_periph, GPIO_MODE_AF_PP, GPIO_OSPEED_50MHZ, uart->tx_pin);
    gpio_init(uart->gpio_periph, GPIO_MODE_IPU, GPIO_OSPEED_50MHZ, uart->rx_pin);

    if (RET_IS_ERR(gd32_uart_apply_config(uart)))
    {
        return RET_INVALID_PARAM;
    }

    gd32_uart_dma_init(uart);

    nvic_irq_enable(uart->uart_irq, uart->irq_preemption_priority, uart->irq_sub_priority);
    nvic_irq_enable(uart->dma_tx_irq, uart->irq_preemption_priority, uart->irq_sub_priority);

    return RET_OK;
}

/*****************************************************************************
@brief: 打开 GD32 UART 设备
@para:dev 设备层传入的 UART 设备对象
@return: RET_OK 表示打开成功，其他返回值表示失败原因
*******************************************************************************/
static int gd32_uart_open(device_t *dev)
{
    gd32_uart_dev_t *uart;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    uart = (gd32_uart_dev_t *)dev->user_data;
    gd32_uart_rx_dma_restart(uart);
    usart_enable(uart->uart_periph);

    return RET_OK;
}

/*****************************************************************************
@brief: 关闭 GD32 UART 设备
@para:dev 设备层传入的 UART 设备对象
@return: RET_OK 表示关闭成功，其他返回值表示失败原因
*******************************************************************************/
static int gd32_uart_close(device_t *dev)
{
    gd32_uart_dev_t *uart;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    uart = (gd32_uart_dev_t *)dev->user_data;
    dma_channel_disable(uart->dma_periph, uart->dma_rx_channel);
    dma_channel_disable(uart->dma_periph, uart->dma_tx_channel);
    usart_disable(uart->uart_periph);
    uart->tx_busy = 0U;

    return RET_OK;
}

/*****************************************************************************
@brief: 通过 TX DMA 提交一帧 UART 发送数据
@para:dev 设备层传入的 UART 设备对象
@para:buf 待发送数据缓冲区
@para:len 待发送数据长度
@return: 正数表示已提交发送的字节数，负数表示失败原因
*******************************************************************************/
static int gd32_uart_write(device_t *dev, const uint8_t *send_buf, uint32_t send_len)
{
    gd32_uart_dev_t *uart;

    if ((dev == 0) || (dev->user_data == 0) || (send_buf == 0) || (send_len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    if (send_len > GD32_UART_FRAME_MAX_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    uart = (gd32_uart_dev_t *)dev->user_data;
    if (uart->tx_busy != 0U)
    {
        return RET_BUSY;
    }

    memcpy(uart->tx_dma_buf, send_buf, send_len);
    uart->tx_busy = 1U;

    dma_channel_disable(uart->dma_periph, uart->dma_tx_channel);
    dma_interrupt_flag_clear(uart->dma_periph, uart->dma_tx_channel, DMA_INT_FLAG_FTF);
    dma_memory_address_config(uart->dma_periph, uart->dma_tx_channel, (uint32_t)uart->tx_dma_buf);
    dma_transfer_number_config(uart->dma_periph, uart->dma_tx_channel, send_len);
    dma_channel_enable(uart->dma_periph, uart->dma_tx_channel);

    return RET_SUCCESS_VALUE(send_len);
}

/*****************************************************************************
@brief: GD32 UART 设备控制接口
@para:dev 设备层传入的 UART 设备对象
@para:cmd UART 控制命令
@para:arg 控制命令参数指针
@return: RET_OK 表示控制成功，其他返回值表示失败原因
*******************************************************************************/
static int gd32_uart_control(device_t *dev, int cmd, void *arg)
{
    gd32_uart_dev_t *uart;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    uart = (gd32_uart_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_UART_CMD_CONFIG:
        if ((arg == 0) || (gd32_uart_config_is_valid((const uart_config_t *)arg) == 0U))
        {
            return RET_INVALID_PARAM;
        }
        uart->config = *(const uart_config_t *)arg;
        dma_channel_disable(uart->dma_periph, uart->dma_rx_channel);
        dma_channel_disable(uart->dma_periph, uart->dma_tx_channel);
        uart->tx_busy = 0U;
        if (RET_IS_ERR(gd32_uart_apply_config(uart)))
        {
            return RET_INVALID_PARAM;
        }
        gd32_uart_dma_init(uart);
        return RET_OK;

    case DEV_UART_CMD_ATTACH_CB:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        uart->callbacks = *(const uart_callback_t *)arg;
        return RET_OK;

    case DEV_UART_CMD_RESTART_RX:
        gd32_uart_rx_dma_restart(uart);
        return RET_OK;

    default:
        return RET_NOT_SUPPORTED;
    }
}

/*****************************************************************************
@brief: GD32 UART 设备操作函数表
*******************************************************************************/
static const device_ops_t s_gd32_uart_ops =
{
    gd32_uart_init,
    gd32_uart_open,
    gd32_uart_close,
    0,
    gd32_uart_write,
    gd32_uart_control,
};

/*****************************************************************************
@brief: 注册一个 GD32 UART 设备
@para:cfg GD32 UART 硬件资源配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败原因
*******************************************************************************/
int gd32_uart_register(const gd32_uart_cfg_t *cfg)
{
    gd32_uart_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->name[0] == '\0') ||
        (gd32_uart_config_is_valid(&cfg->default_config) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (s_uart_count >= GD32_UART_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    if (device_find(cfg->name) != 0)
    {
        return RET_ALREADY_EXISTS;
    }

    slot = &s_uart_slots[s_uart_count];
    memset(slot, 0, sizeof(*slot));

    slot->uart.uart_periph = cfg->uart_periph;
    slot->uart.uart_rcu = cfg->uart_rcu;
    slot->uart.gpio_rcu = cfg->gpio_rcu;
    slot->uart.gpio_periph = cfg->gpio_periph;
    slot->uart.tx_pin = cfg->tx_pin;
    slot->uart.rx_pin = cfg->rx_pin;
    slot->uart.uart_irq = cfg->uart_irq;
    slot->uart.dma_periph = cfg->dma_periph;
    slot->uart.dma_rcu = cfg->dma_rcu;
    slot->uart.dma_rx_channel = cfg->dma_rx_channel;
    slot->uart.dma_tx_channel = cfg->dma_tx_channel;
    slot->uart.dma_tx_irq = cfg->dma_tx_irq;
    slot->uart.irq_preemption_priority = cfg->irq_preemption_priority;
    slot->uart.irq_sub_priority = cfg->irq_sub_priority;
    slot->uart.config = cfg->default_config;

    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_UART;
    slot->dev.ops = &s_gd32_uart_ops;
    slot->dev.user_data = &slot->uart;
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_uart_count++;
    }

    return ret;
}

/*****************************************************************************
@brief: GD32 UART IDLE 中断统一分发函数
@para:uart_periph 触发中断的 USART/UART 外设基地址
@return:
@note: 驱动层只计算 DMA 接收长度并调用上层回调，不直接依赖 RTOS。
*******************************************************************************/
static void gd32_uart_irq_dispatch(uint32_t uart_periph)
{
    gd32_uart_slot_t *slot;
    gd32_uart_dev_t *uart;
    uint32_t rx_len;

    slot = gd32_uart_find_slot_by_periph(uart_periph);
    if (slot == 0)
    {
        return;
    }

    uart = &slot->uart;
    if (usart_interrupt_flag_get(uart->uart_periph, USART_INT_FLAG_IDLE) == RESET)
    {
        return;
    }

    //清标志位
    usart_interrupt_flag_clear(uart->uart_periph, USART_INT_FLAG_IDLE);
    (void)usart_data_receive(uart->uart_periph);

    dma_channel_disable(uart->dma_periph, uart->dma_rx_channel);
    rx_len = GD32_UART_FRAME_MAX_LEN - dma_transfer_number_get(uart->dma_periph, uart->dma_rx_channel);
    if ((rx_len > 0U) && (rx_len <= GD32_UART_FRAME_MAX_LEN) && (uart->callbacks.rx_cb != 0))
    {
        uart->callbacks.rx_cb(slot->dev.name, uart->rx_dma_buf, rx_len,  uart->callbacks.user_ctx);
    }

    gd32_uart_rx_dma_restart(uart);
}

/*****************************************************************************
@brief: GD32 UART TX DMA 完成中断统一分发函数
@para:dma_irq 触发中断的 DMA IRQn
@return:
@note: 驱动层只清除 DMA 标志并调用上层发送完成回调，不直接依赖 RTOS。
*******************************************************************************/
static void gd32_uart_dma_tx_irq_dispatch(IRQn_Type dma_irq)
{
    gd32_uart_dev_t *uart;
    uint8_t i;

    for (i = 0U; i < s_uart_count; i++)
    {
        uart = &s_uart_slots[i].uart;
        if (uart->dma_tx_irq != dma_irq)
        {
            continue;
        }

        if (dma_interrupt_flag_get(uart->dma_periph, uart->dma_tx_channel, DMA_INT_FLAG_FTF) != RESET)
        {
            dma_interrupt_flag_clear(uart->dma_periph, uart->dma_tx_channel, DMA_INT_FLAG_FTF);
            dma_channel_disable(uart->dma_periph, uart->dma_tx_channel);
            uart->tx_busy = 0U;
            if (uart->callbacks.tx_done_cb != 0)
            {
                uart->callbacks.tx_done_cb(s_uart_slots[i].dev.name, uart->callbacks.user_ctx);
            }
        }
    }
}

/*****************************************************************************
@brief: UART3 全局中断入口
@para:
@return:
*******************************************************************************/
void UART3_IRQHandler(void)
{
    gd32_uart_irq_dispatch(UART3);
}

/*****************************************************************************
@brief: DMA1 Channel3/4 共用中断入口，当前用于 UART3 TX DMA 完成中断
@para:
@return:
*******************************************************************************/
void DMA1_Channel3_4_IRQHandler(void)
{
    gd32_uart_dma_tx_irq_dispatch(DMA1_Channel3_Channel4_IRQn);
}
