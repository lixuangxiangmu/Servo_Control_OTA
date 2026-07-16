#ifndef DRV_PWM_H
#define DRV_PWM_H

#include "dev_pwm.h"
#include "gd32f10x_libopt.h"

#include <stdint.h>

/* GD32 PWM 硬件资源配置，由 BSP 板级配置层提供。 */
typedef struct
{
    const char *name;                 /* 注册到设备层的 PWM 设备名称。 */
    rcu_periph_enum timer_rcu;        /* 定时器外设时钟枚举。 */
    rcu_periph_enum gpio_rcu;         /* PWM 输出引脚 GPIO 时钟枚举。 */
    uint32_t timer_periph;            /* 定时器外设基地址，例如 TIMER7。 */
    uint16_t channel;                 /* PWM 输出通道，例如 TIMER_CH_0。 */
    uint32_t gpio_periph;             /* GPIO 外设基地址，例如 GPIOC。 */
    uint32_t gpio_pin;                /* GPIO 引脚掩码，例如 GPIO_PIN_6。 */
    uint32_t gpio_mode;               /* GPIO 工作模式，PWM 输出通常为复用推挽输出。 */
    uint32_t gpio_speed;              /* GPIO 输出速度。 */
    uint32_t source_clock_hz;          /* 定时器源时钟，0 表示使用 SystemCoreClock。 */
    pwm_config_t default_config;       /* PWM 默认运行参数。 */
} gd32_pwm_cfg_t;

/*****************************************************************************
@brief: 注册一个 GD32 PWM 设备
@para:cfg PWM 硬件资源配置
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int gd32_pwm_register(const gd32_pwm_cfg_t *cfg);

#endif
