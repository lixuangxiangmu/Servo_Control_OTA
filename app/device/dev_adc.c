#include "dev_adc.h"

#include "device.h"
#include "return_code.h"

/*****************************************************************************
@brief: 按名称查找 ADC 设备，并确认设备类型确实为 ADC
@para:name ADC 设备名
@return: 找到合法 ADC 设备时返回 device_t 指针，否则返回空指针
@note:
  这里是 device 层和具体 ADC 设备之间的第一道保护。
  因为 device_find 只按名字查找，如果调用者把 PWM/GPIO 名称传进来，
  必须通过 DEVICE_CLASS_ADC 拦住，避免后续错误地调用 ADC 驱动接口。
*******************************************************************************/
static device_t *dev_adc_find(const char *name)
{
    device_t *dev;

    if (name == 0)
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_ADC))
    {
        return 0;
    }

    return dev;
}

/*****************************************************************************
@brief: 配置指定名称的 ADC 设备
@para:name ADC 设备名
@para:cfg ADC 通用配置参数
@return: RET_OK 表示成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在或设备类型不是 ADC
@note:
  当前 GD32 第一版驱动只实现 raw 读取，动态配置接口暂时会由底层返回
  RET_NOT_SUPPORTED。保留这个接口是为了以后扩展采样时间、触发源等能力。
*******************************************************************************/
int dev_adc_config(const char *name, const adc_config_t *cfg)
{
    device_t *dev;
    int ret;

    if ((cfg == 0) || (cfg->mode > ADC_DEV_MODE_SCAN))
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_adc_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    /* 设备层不理解具体 ADC 寄存器，只把通用配置透传到底层 control。 */
    ret = device_control(dev, DEV_ADC_CMD_CONFIG, (void *)cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 按设备名读取最新一次 ADC raw 原始采样值
@para:name ADC 设备名
@para:raw 输出参数，用于保存 raw 原始采样值
@return: RET_OK 表示成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NOT_FOUND 表示设备不存在或设备类型不是 ADC
@note:
  这里读取的是底层驱动维护的最新 raw 值，不做 mV 换算，也不做分压比例换算。
  上层服务如果需要电压、电流、温度等物理量，应在 service 层自行转换。
*******************************************************************************/
int dev_adc_read_raw(const char *name, uint16_t *raw)
{
    device_t *dev;
    uint16_t value = 0U;
    int ret;

    if (raw == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_adc_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    /* device_read 会继续分发到 dev->ops->read，也就是 GD32 ADC 驱动的 read 函数。 */
    ret = device_read(dev, (uint8_t *)&value, (uint32_t)sizeof(value));
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    *raw = value;

    return RET_OK;
}
