#ifndef DEV_ADC_H
#define DEV_ADC_H

#include <stdint.h>

/* ADC 设备层控制命令：通过 device_control 分发到底层驱动。 */
#define DEV_ADC_CMD_CONFIG      1

/* ADC 工作模式定义。
   当前第一版驱动主要使用扫描模式，单通道模式先作为接口预留。 */
typedef enum
{
    ADC_DEV_MODE_SINGLE = 0,
    ADC_DEV_MODE_SCAN,
} adc_mode_t;

/* ADC 通用配置结构体。
   该结构体属于 device 设备抽象层，不直接暴露 GD32 寄存器枚举。
   当前第一版 raw 读取功能暂未动态修改采样时间、分辨率和过采样参数。 */
typedef struct
{
    adc_mode_t mode;          /* ADC 工作模式：单通道或扫描。 */
    uint32_t sample_time_ns;   /* 期望采样时间，单位 ns；不支持动态配置时由底层忽略。 */
    uint8_t resolution_bits;   /* 期望分辨率，例如 12bit；GD32F10x 当前固定按 12bit 使用。 */
    uint8_t oversample;        /* 过采样倍率；不支持时由底层忽略。 */
} adc_config_t;

/*****************************************************************************
@brief: 配置指定名称的 ADC 设备
@para:name ADC 设备名，由 BSP 注册，例如 "adc_power_raw"
@para:cfg ADC 通用配置参数
@return: RET_OK 表示成功，其他返回值表示失败
*******************************************************************************/
int dev_adc_config(const char *name, const adc_config_t *cfg);

/*****************************************************************************
@brief: 按设备名读取最新一次 ADC raw 原始采样值
@para:name ADC 设备名，由 BSP 注册，例如 "adc_power_raw"
@para:raw 输出参数，用于保存 12bit ADC 原始值，范围通常为 0~4095
@return: RET_OK 表示成功，其他返回值表示失败
*******************************************************************************/
int dev_adc_read_raw(const char *name, uint16_t *raw);

#endif
