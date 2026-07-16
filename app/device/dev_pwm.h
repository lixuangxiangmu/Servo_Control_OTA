#ifndef DEV_PWM_H
#define DEV_PWM_H

#include <stdint.h>

/* PWM 输出极性定义。 */
typedef enum
{
    PWM_POLARITY_NORMAL = 0,    /* 正常极性，高电平为有效脉冲。 */
    PWM_POLARITY_INVERTED,      /* 反向极性，低电平为有效脉冲。 */
} pwm_polarity_t;

/* PWM 运行参数配置。 */
typedef struct
{
    uint32_t freq_hz;           /* PWM 频率，单位 Hz。 */
    uint16_t duty_permille;     /* PWM 占空比，千分比，取值范围 0 到 1000。 */
    pwm_polarity_t polarity;    /* PWM 输出极性。 */
} pwm_config_t;

/* PWM 设备控制命令定义，用于 device_control 分发到底层驱动。 */
#define DEV_PWM_CMD_CONFIG              1
#define DEV_PWM_CMD_SET_FREQ            2
#define DEV_PWM_CMD_SET_DUTY            3
#define DEV_PWM_CMD_GET_DUTY            4
#define DEV_PWM_CMD_SET_PULSE_WIDTH_US  5

/*****************************************************************************
@brief: 配置 PWM 输出参数
@para:name PWM 设备名称
@para:cfg PWM 配置参数
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_config(const char *name, const pwm_config_t *cfg);

/*****************************************************************************
@brief: 设置 PWM 输出频率
@para:name PWM 设备名称
@para:freq_hz 目标频率，单位 Hz
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_set_freq(const char *name, uint32_t freq_hz);

/*****************************************************************************
@brief: 设置 PWM 输出占空比
@para:name PWM 设备名称
@para:duty_permille 目标占空比，千分比，取值范围 0 到 1000
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_set_duty(const char *name, uint16_t duty_permille);

/*****************************************************************************
@brief: 获取当前 PWM 输出占空比
@para:name PWM 设备名称
@para:duty_permille 输出参数，用于保存当前占空比，单位为千分比
@return: RET_OK 表示获取成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_get_duty(const char *name, uint16_t *duty_permille);

/*****************************************************************************
@brief: 直接设置 PWM 有效脉宽
@para:name PWM 设备名称
@para:pulse_width_us 目标有效脉宽，单位 us
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_set_pulse_width_us(const char *name, uint32_t pulse_width_us);

/*****************************************************************************
@brief: 启动 PWM 输出
@para:name PWM 设备名称
@return: RET_OK 表示启动成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_start(const char *name);

/*****************************************************************************
@brief: 停止 PWM 输出
@para:name PWM 设备名称
@return: RET_OK 表示停止成功，其他返回值表示失败
*******************************************************************************/
int dev_pwm_stop(const char *name);

#endif
