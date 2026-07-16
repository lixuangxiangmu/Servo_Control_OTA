/*****************************************************************************
 * @file    app_hw.c
 * @brief   硬件抽象层（HAL）实现
 *          为上层应用模块提供统一的硬件操作接口，封装底层器件驱动的调用细节。
 *          当前封装的外设包括：
 *            - GPIO：电源保持引脚（Power Hold），控制系统自锁供电电路
 *            - ADC： 两路 ADC 通道 —— 电源电压采样、堵转电流采样
 *            - UART：蓝牙透传串口（UART3），支持字节读写和字符串发送
 *
 *          设计原则：
 *            - 薄封装层，不添加业务逻辑，仅做参数校验和接口适配
 *            - 所有接口返回统一错误码（return_code.h），便于上层统一错误处理
 *            - 电源电压换算公式沿用老工程经验值，保证兼容性
 * @author  LXA
 * @version v1.0
 * @date    2026-06-17
 *****************************************************************************/

#include "app_hw.h"

/* 硬件器件驱动层接口 */
#include "board_config.h"   /* 板级引脚/通道名称宏定义 */
#include "dev_adc.h"        /* ADC 驱动（原始值读取） */
#include "dev_gpio.h"       /* GPIO 驱动（电平控制） */
#include "dev_uart.h"       /* UART 驱动（串口收发） */
#include "return_code.h"    /* 统一返回码（RET_OK / RET_INVALID_PARAM 等） */

#include <stdint.h>
#include <string.h>         /* strlen() */

/*===========================================================================
 * 电源管理接口
 *===========================================================================*/

/*****************************************************************************
 * @brief:  设置电源保持引脚电平（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_power_hold_set(uint8_t enable)
{
    /* 将 enable 转换为 GPIO 电平：非零值 -> 高电平，零值 -> 低电平 */
    return dev_gpio_write(BOARD_SERVO_POWER_HOLD_NAME, (enable != 0U) ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
}

/*****************************************************************************
 * @brief:  读取供电电压并换算为毫伏值（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_read_power_voltage_mv(uint32_t *voltage_mv)
{
    uint16_t raw;              /* ADC 原始采样值（12 位，0~4095） */
    uint32_t converted_mv;     /* 换算后的电压值（毫伏） */
    int ret;

    /* 参数有效性检查 */
    if (voltage_mv == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* 读取电源电压 ADC 通道原始值 */
    ret = dev_adc_read_raw(BOARD_ADC_POWER_RAW_NAME, &raw);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 沿用老工程换算：ADC 电压 × 11，再补偿 -200mV。这里用整数运算避免浮点依赖。 */
    converted_mv = ((uint32_t)raw * 36300U) / 4095U;
    /* 补偿值为 200mV：若换算结果小于补偿值则返回 0（避免下溢） */
    *voltage_mv = (converted_mv > 200U) ? (converted_mv - 200U) : 0U;

    return RET_OK;
}

/*===========================================================================
 * 堵转检测接口
 *===========================================================================*/

/*****************************************************************************
 * @brief:  读取堵转电流 ADC 原始采样值（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_read_locked_rotor_raw(uint32_t *raw)
{
    uint16_t adc_raw;          /* ADC 原始采样值（16 位临时存储） */
    int ret;

    /* 参数有效性检查 */
    if (raw == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* 读取堵转电流 ADC 通道原始值 */
    ret = dev_adc_read_raw(BOARD_ADC_LOCKED_ROTOR_RAW_NAME, &adc_raw);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 将 16 位 ADC 值扩展到 32 位，方便上层统一处理 */
    *raw = (uint32_t)adc_raw;

    return RET_OK;
}

/*===========================================================================
 * 蓝牙通信接口
 *===========================================================================*/

/*****************************************************************************
 * @brief:  从蓝牙串口读取一个字节（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_bluetooth_read(uint8_t *buf, uint32_t timeout_ticks)
{
    /* 直接委托给 UART 驱动，不做额外处理 */
    return dev_uart_read(BOARD_UART3_NAME, buf, timeout_ticks);
}

/*****************************************************************************
 * @brief:  向蓝牙串口写入指定长度数据（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_bluetooth_write(const uint8_t *buf, uint32_t len, uint32_t timeout_ticks)
{
    /* 直接委托给 UART 驱动，不做额外处理 */
    return dev_uart_write(BOARD_UART3_NAME, buf, len, timeout_ticks);
}

/*****************************************************************************
 * @brief:  向蓝牙串口发送字符串（见 app_hw.h 详细说明）
 *****************************************************************************/
int app_hw_bluetooth_write_text(const char *text)
{
    /* 参数有效性检查 */
    if (text == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* 自动计算字符串长度，使用默认超时时间发送 */
    return app_hw_bluetooth_write((const uint8_t *)text, (uint32_t)strlen(text), DEV_UART_DEFAULT_TIMEOUT_TICKS);
}
