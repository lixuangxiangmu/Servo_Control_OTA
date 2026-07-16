#include "servo_service.h"

#include "dev_pwm.h"
#include "return_code.h"

/* 舵机标准 PWM 频率，20ms 周期。 */
#define SERVO_DEFAULT_FREQ_HZ 50U

/* 应用层注入的舵机硬件绑定配置表。 */
static const servo_service_item_cfg_t *s_servo_items;

/* 舵机配置数量。 */
static uint8_t s_servo_item_count;

/* 舵机最后一次设置的角度缓存。 */
static uint8_t s_servo_angle[SERVO_ID_MAX];

/*****************************************************************************
@brief: 检查舵机逻辑 ID 是否有效
@para:servo_id 舵机逻辑 ID
@return: 1 表示有效，0 表示无效
*******************************************************************************/
static int servo_id_is_valid(servo_id_t servo_id)
{
    return ((servo_id < SERVO_ID_MAX) && (s_servo_items != 0) &&
            ((uint8_t)servo_id < s_servo_item_count) &&
            (s_servo_items[servo_id].pwm_name != 0)) ? 1 : 0;
}

/*****************************************************************************
@brief: 将舵机角度转换为 PWM 有效脉宽
@para:item 舵机配置项
@para:angle 目标角度
@return: 目标角度对应的 PWM 有效脉宽，单位 us
*******************************************************************************/
static uint32_t servo_angle_to_pulse_us(const servo_service_item_cfg_t *item, uint8_t angle)
{
    uint32_t angle_span;
    uint32_t pulse_span;

    angle_span = (uint32_t)(item->max_angle - item->min_angle);
    pulse_span = (uint32_t)(item->max_pulse_us - item->min_pulse_us);

    if (angle_span == 0U)
    {
        return item->min_pulse_us;
    }

    return (uint32_t)item->min_pulse_us +
           (((uint32_t)(angle - item->min_angle) * pulse_span) / angle_span);
}

/*****************************************************************************
@brief: 初始化舵机服务
@para:cfg 产品舵机绑定配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int servo_service_init(const servo_service_cfg_t *cfg)
{
    pwm_config_t pwm_cfg;
    int ret;
    uint8_t i;

    if ((cfg == 0) || (cfg->items == 0) || (cfg->item_count == 0U) ||
        (cfg->item_count > (uint8_t)SERVO_ID_MAX))
    {
        return RET_INVALID_PARAM;
    }

    s_servo_items = cfg->items;
    s_servo_item_count = cfg->item_count;

    for (i = 0U; i < s_servo_item_count; i++)
    {
        if ((s_servo_items[i].min_angle >= s_servo_items[i].max_angle) ||
            (s_servo_items[i].min_pulse_us >= s_servo_items[i].max_pulse_us) ||
            ((s_servo_items[i].init_pulse_us != 0U) &&
             ((s_servo_items[i].init_pulse_us < s_servo_items[i].min_pulse_us) ||
              (s_servo_items[i].init_pulse_us > s_servo_items[i].max_pulse_us))) ||
            (s_servo_items[i].init_angle < s_servo_items[i].min_angle) ||
            (s_servo_items[i].init_angle > s_servo_items[i].max_angle))
        {
            return RET_INVALID_PARAM;
        }

        pwm_cfg.freq_hz = SERVO_DEFAULT_FREQ_HZ;
        pwm_cfg.duty_permille = 0U;
        pwm_cfg.polarity = PWM_POLARITY_NORMAL;

        ret = dev_pwm_config(s_servo_items[i].pwm_name, &pwm_cfg);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = dev_pwm_set_pulse_width_us(s_servo_items[i].pwm_name,
                                         (s_servo_items[i].init_pulse_us != 0U) ?
                                         s_servo_items[i].init_pulse_us :
                                         servo_angle_to_pulse_us(&s_servo_items[i],
                                                                 s_servo_items[i].init_angle));
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = dev_pwm_start(s_servo_items[i].pwm_name);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        s_servo_angle[i] = s_servo_items[i].init_angle;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 设置 PWM 舵机目标角度
@para:servo_id 舵机逻辑 ID
@para:angle 目标角度
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int servo_service_set_angle(servo_id_t servo_id, uint8_t angle)
{
    const servo_service_item_cfg_t *item;
    uint32_t pulse_us;
    int ret;

    if (servo_id_is_valid(servo_id) == 0)
    {
        return RET_INVALID_PARAM;
    }

    item = &s_servo_items[servo_id];
    if ((angle < item->min_angle) || (angle > item->max_angle))
    {
        return RET_INVALID_PARAM;
    }

    pulse_us = servo_angle_to_pulse_us(item, angle);
    ret = dev_pwm_set_pulse_width_us(item->pwm_name, pulse_us);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    s_servo_angle[servo_id] = angle;

    return RET_OK;
}

/*****************************************************************************
@brief: 获取 PWM 舵机最后一次设置的角度
@para:servo_id 舵机逻辑 ID
@para:angle 输出参数，用于保存最后一次设置的角度
@return: RET_OK 表示获取成功，其他返回值表示失败
*******************************************************************************/
int servo_service_get_angle(servo_id_t servo_id, uint8_t *angle)
{
    if ((servo_id_is_valid(servo_id) == 0) || (angle == 0))
    {
        return RET_INVALID_PARAM;
    }

    *angle = s_servo_angle[servo_id];

    return RET_OK;
}
