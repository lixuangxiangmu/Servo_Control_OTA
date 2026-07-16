#ifndef DRV_ADC_H
#define DRV_ADC_H

#include "gd32f10x_libopt.h"

#include <stdint.h>

/* 单个 ADC 逻辑通道的板级硬件描述。
   一个物理 ADC 扫描组可以包含多个逻辑通道，每个逻辑通道最终会注册成
   一个可按名称查找的 device，例如 "adc_power_raw"。 */
typedef struct
{
    const char *name;          /* 注册到 device 层的 ADC 设备名。 */
    rcu_periph_enum gpio_rcu;  /* ADC 输入引脚所在 GPIO 端口时钟。 */
    uint32_t gpio_periph;      /* ADC 输入引脚所在 GPIO 端口，例如 GPIOB。 */
    uint32_t gpio_pin;         /* ADC 输入引脚，例如 GPIO_PIN_0。 */
    uint8_t adc_channel;       /* GD32 ADC 通道号，例如 ADC_CHANNEL_8。 */
    uint32_t sample_time;      /* GD32 ADC 采样周期枚举，例如 ADC_SAMPLETIME_239POINT5。 */
} gd32_adc_channel_cfg_t;

/* 一个 GD32 ADC 扫描组的硬件描述。
   当前板级配置中，ADC0 + DMA0_CH0 作为一个扫描组，组内包含 PB0/PB1 两路。 */
typedef struct
{
    uint32_t adc_periph;                         /* ADC 外设基地址，例如 ADC0。 */
    rcu_periph_enum adc_rcu;                     /* ADC 外设时钟，例如 RCU_ADC0。 */
    uint32_t adc_clock_div;                      /* ADC 时钟分频，例如 RCU_CKADC_CKAPB2_DIV6。 */
    uint32_t dma_periph;                         /* DMA 外设基地址，例如 DMA0。 */
    rcu_periph_enum dma_rcu;                     /* DMA 外设时钟，例如 RCU_DMA0。 */
    dma_channel_enum dma_channel;                /* DMA 通道，例如 DMA_CH0。 */
    const gd32_adc_channel_cfg_t *channels;      /* 扫描组内的逻辑通道数组。 */
    uint8_t channel_count;                       /* 逻辑通道数量。 */
} gd32_adc_cfg_t;

/*****************************************************************************
@brief: 注册一个 GD32 ADC 扫描组及其逻辑 ADC 通道设备
@para:cfg ADC 硬件资源配置，由 BSP 板级配置层提供
@return: RET_OK 表示成功，其他返回值表示失败
*******************************************************************************/
int gd32_adc_register(const gd32_adc_cfg_t *cfg);

#endif
