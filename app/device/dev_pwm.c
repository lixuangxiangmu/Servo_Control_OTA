#include "dev_pwm.h"

#include "device.h"
#include "return_code.h"

/*****************************************************************************
@brief: 按名称查找 PWM 设备
@para:name PWM 设备名称
@return: 查找到 PWM 设备时返回设备指针，否则返回空指针
*******************************************************************************/
static device_t *dev_pwm_find(const char *name)
{
    device_t *dev;

    if (name == 0)
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_PWM))
    {
        return 0;
    }

    return dev;
}

/*****************************************************************************
@brief: 配置 PWM 输出参数
@para:name PWM 设备名称
@para:cfg PWM 配置参数
@return: RET_OK 表示配置成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_config(const char *name, const pwm_config_t *cfg)
{
    device_t *dev;
    int ret;

    if ((cfg == 0) || (cfg->freq_hz == 0U) || (cfg->duty_permille > 1000U) ||
        (cfg->polarity > PWM_POLARITY_INVERTED))
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_PWM_CMD_CONFIG, (void *)cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 设置 PWM 输出频率
@para:name PWM 设备名称
@para:freq_hz 目标频率，单位 Hz
@return: RET_OK 表示设置成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_set_freq(const char *name, uint32_t freq_hz)
{
    device_t *dev;
    int ret;

    if (freq_hz == 0U)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_PWM_CMD_SET_FREQ, &freq_hz);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 设置 PWM 输出占空比
@para:name PWM 设备名称
@para:duty_permille 目标占空比，千分比，取值范围 0 到 1000
@return: RET_OK 表示设置成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_set_duty(const char *name, uint16_t duty_permille)
{
    device_t *dev;
    int ret;

    if (duty_permille > 1000U)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_PWM_CMD_SET_DUTY, &duty_permille);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 获取当前 PWM 输出占空比
@para:name PWM 设备名称
@para:duty_permille 输出参数，用于保存当前占空比，单位为千分比
@return: RET_OK 表示获取成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_get_duty(const char *name, uint16_t *duty_permille)
{
    device_t *dev;
    int ret;

    if (duty_permille == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_PWM_CMD_GET_DUTY, duty_permille);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 直接设置 PWM 有效脉宽
@para:name PWM 设备名称
@para:pulse_width_us 目标有效脉宽，单位 us
@return: RET_OK 表示设置成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_set_pulse_width_us(const char *name, uint32_t pulse_width_us)
{
    device_t *dev;
    int ret;

    if (pulse_width_us == 0U)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_PWM_CMD_SET_PULSE_WIDTH_US, &pulse_width_us);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 启动 PWM 输出
@para:name PWM 设备名称
@return: RET_OK 表示启动成功，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_start(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_open(dev);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 停止 PWM 输出
@para:name PWM 设备名称
@return: RET_OK 表示停止成功，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_pwm_stop(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_pwm_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_close(dev);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}
