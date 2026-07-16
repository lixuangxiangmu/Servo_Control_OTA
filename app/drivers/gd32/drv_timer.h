#ifndef DRV_TIMER_H
#define DRV_TIMER_H

#include "dev_timer.h"
#include "gd32f10x_libopt.h"

#include <stdint.h>

/* GD32 周期定时器硬件资源配置，由 BSP 板级配置层提供。 */
typedef struct
{
    const char *name;               /* 注册到 device 层的定时器设备名称。 */
    rcu_periph_enum timer_rcu;      /* 定时器外设时钟枚举，例如 RCU_TIMER4。 */
    uint32_t timer_periph;          /* 定时器外设基地址，例如 TIMER4。 */
    IRQn_Type irqn;                 /* 定时器更新中断号，例如 TIMER4_IRQn。 */
    uint32_t source_clock_hz;       /* 定时器源时钟，0 表示根据 APB 总线自动计算。 */
    uint8_t preemption_priority;    /* NVIC 抢占优先级。 */
    uint8_t sub_priority;           /* NVIC 子优先级。 */
    timer_config_t default_config;  /* 定时器默认周期配置。 */
} gd32_timer_cfg_t;

/*****************************************************************************
@brief: 注册一个 GD32 周期定时器设备
@para:cfg 定时器硬件资源配置
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int gd32_timer_register(const gd32_timer_cfg_t *cfg);

#endif
