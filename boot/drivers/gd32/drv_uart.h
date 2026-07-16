#ifndef DRV_UART_H
#define DRV_UART_H

#include "dev_uart.h"
#include "gd32f10x_libopt.h"

#include <stdint.h>

#define GD32_UART_FRAME_MAX_LEN     DEV_UART_FRAME_MAX_LEN

/* GD32 UART 硬件资源配置，用于在 BSP 中描述一个板级 UART 逻辑设备。 */
typedef struct
{
    const char *name;                       /* UART 逻辑设备名称 */
    uint32_t uart_periph;                   /* USART/UART 外设基地址 */
    rcu_periph_enum uart_rcu;               /* USART/UART 外设时钟 */
    rcu_periph_enum gpio_rcu;               /* TX/RX 所在 GPIO 端口时钟 */
    uint32_t gpio_periph;                   /* TX/RX 所在 GPIO 端口 */
    uint32_t tx_pin;                        /* UART TX 引脚 */
    uint32_t rx_pin;                        /* UART RX 引脚 */
    IRQn_Type uart_irq;                     /* UART 全局中断号 */
    uint32_t dma_periph;                    /* DMA 外设基地址 */
    rcu_periph_enum dma_rcu;                /* DMA 外设时钟 */
    dma_channel_enum dma_rx_channel;        /* UART RX DMA 通道 */
    dma_channel_enum dma_tx_channel;        /* UART TX DMA 通道 */
    IRQn_Type dma_tx_irq;                   /* UART TX DMA 完成中断号 */
    uint8_t irq_preemption_priority;        /* NVIC 抢占优先级 */
    uint8_t irq_sub_priority;               /* NVIC 子优先级 */
    uart_config_t default_config;           /* 默认串口参数 */
} gd32_uart_cfg_t;

/*****************************************************************************
@brief: 注册一个 GD32 UART 设备
@para:cfg GD32 UART 硬件资源配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败原因
*******************************************************************************/
int gd32_uart_register(const gd32_uart_cfg_t *cfg);

#endif
