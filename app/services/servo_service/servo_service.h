#ifndef SERVO_SERVICE_H
#define SERVO_SERVICE_H

#include <stdint.h>

/* 舵机逻辑 ID 定义。 */
typedef enum
{
    SERVO_MAIN = 0,     /* 主舵机。 */
    SERVO_ID_MAX,       /* 舵机数量边界值，不作为实际舵机使用。 */
} servo_id_t;

/* 单个舵机绑定配置，由应用配置层提供。 */
typedef struct
{
    const char *pwm_name;       /* 舵机绑定的 PWM 设备名称。 */
    uint16_t min_pulse_us;      /* 最小角度对应的脉宽，单位 us。 */
    uint16_t max_pulse_us;      /* 最大角度对应的脉宽，单位 us。 */
    uint16_t init_pulse_us;     /* 初始化脉宽，0 表示按初始化角度自动计算。 */
    uint8_t min_angle;          /* 舵机允许的最小角度。 */
    uint8_t max_angle;          /* 舵机允许的最大角度。 */
    uint8_t init_angle;         /* 舵机初始化角度。 */
} servo_service_item_cfg_t;

/* 舵机服务配置，由 application/app_config.c 提供。 */
typedef struct
{
    const servo_service_item_cfg_t *items;  /* 舵机配置表。 */
    uint8_t item_count;                     /* 舵机配置数量。 */
} servo_service_cfg_t;

/*****************************************************************************
@brief: 初始化舵机服务
@para:cfg 产品舵机绑定配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int servo_service_init(const servo_service_cfg_t *cfg);

/*****************************************************************************
@brief: 设置 PWM 舵机目标角度
@para:servo_id 舵机逻辑 ID
@para:angle 目标角度
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int servo_service_set_angle(servo_id_t servo_id, uint8_t angle);

/*****************************************************************************
@brief: 获取 PWM 舵机最后一次设置的角度
@para:servo_id 舵机逻辑 ID
@para:angle 输出参数，用于保存最后一次设置的角度
@return: RET_OK 表示获取成功，其他返回值表示失败
*******************************************************************************/
int servo_service_get_angle(servo_id_t servo_id, uint8_t *angle);

#endif
