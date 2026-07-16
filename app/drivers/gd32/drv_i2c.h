#ifndef DRV_I2C_H
#define DRV_I2C_H

#include "dev_i2c.h"
#include "gd32f10x_libopt.h"

#include <stdint.h>

/* GD32 I2C 硬件资源配置，用于注册到设备框架。 */
typedef struct
{
    const char *name;                   /* I2C 设备名称 */
    uint32_t i2c_periph;                /* GD32 I2C 外设基地址标识 */
    rcu_periph_enum i2c_rcu;            /* I2C 外设时钟 */
    rcu_periph_enum scl_gpio_rcu;       /* SCL GPIO 端口时钟 */
    uint32_t scl_gpio_periph;           /* SCL GPIO 端口 */
    uint32_t scl_gpio_pin;              /* SCL GPIO 引脚 */
    rcu_periph_enum sda_gpio_rcu;       /* SDA GPIO 端口时钟 */
    uint32_t sda_gpio_periph;           /* SDA GPIO 端口 */
    uint32_t sda_gpio_pin;              /* SDA GPIO 引脚 */
    i2c_config_t default_config;        /* 默认 I2C 总线配置 */
} gd32_i2c_cfg_t;

/*****************************************************************************
@brief: 注册一个 GD32 I2C 设备
@para:cfg GD32 I2C 硬件资源配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int gd32_i2c_register(const gd32_i2c_cfg_t *cfg);

#endif
