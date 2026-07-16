#include "dev_gpio.h"

#include "device.h"
#include "return_code.h"

#include <stdint.h>

/*****************************************************************************
@brief: Set the GPIO working mode
@para:name GPIO device name
@para:mode Target GPIO working mode
@return: RET_OK indicates success,
         RET_INVALID_PARAM indicates an invalid parameter,
         RET_NOT_FOUND indicates the device does not exist
*******************************************************************************/
int dev_gpio_set_mode(const char *name, gpio_mode_t mode)
{
    device_t *dev;
    int ret;

    if ((name == 0) || (mode > GPIO_DEV_MODE_OUTPUT_OD))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_GPIO_CMD_SET_MODE, &mode);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: Set the GPIO output level
@para:name GPIO device name
@para:level Target output level
@return: RET_OK indicates success,
         RET_INVALID_PARAM indicates an invalid parameter,
         RET_NOT_FOUND indicates the device does not exist
*******************************************************************************/
int dev_gpio_write(const char *name, gpio_level_t level)
{
    device_t *dev;
    uint8_t value;
    int ret;

    if ((name == 0) || (level > GPIO_LEVEL_HIGH))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    value = (uint8_t)level;
    ret = device_write(dev, &value, 1U);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: Read the current GPIO input level
@para:name GPIO device name
@para:level Output parameter, used to store the read level
@return: RET_OK indicates read success,
         RET_INVALID_PARAM indicates an invalid parameter,
         RET_NOT_FOUND indicates the device does not exist
*******************************************************************************/
int dev_gpio_read(const char *name, gpio_level_t *level)
{
    device_t *dev;
    uint8_t value = 0U;
    int ret;

    if ((name == 0) || (level == 0))
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_read(dev, &value, 1U);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    *level = (value != 0U) ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;

    return RET_OK;
}

/*****************************************************************************
@brief: Toggle the current GPIO output level
@para:name GPIO device name
@return: RET_OK indicates toggle success, other return values indicate failure
*******************************************************************************/
int dev_gpio_toggle(const char *name)
{
    gpio_level_t level;
    int ret;

    ret = dev_gpio_read(name, &level);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    level = (level == GPIO_LEVEL_HIGH) ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;

    return dev_gpio_write(name, level);
}

/*****************************************************************************
@brief: 给 GPIO 输入绑定中断回调
@para:name GPIO 逻辑设备名
@para:trigger 中断触发方式
@para:cb 中断回调函数，最终会在 EXTI ISR 中被调用
@para:user_ctx 用户私有上下文，驱动层不解析，仅透传给 cb
@return: RET_OK 表示成功，
         RET_INVALID_PARAM 表示参数非法，
         RET_NOT_FOUND 表示设备不存在
@note: 该函数属于 device 抽象层，只负责查找设备并把配置通过 control 下发给具体驱动。
       具体的 GPIO/EXTI/NVIC 配置由 drivers/gd32/drv_gpio.c 完成。
*******************************************************************************/
int dev_gpio_attach_irq(const char *name, gpio_irq_trigger_t trigger, gpio_irq_cb_t cb, void *user_ctx)
{
    device_t *dev;
    gpio_irq_config_t cfg_irq;
    int ret;

    /* 中断必须有明确触发边沿和有效回调，否则底层无法安全上报事件。 */
    if ((name == 0) || (trigger == GPIO_IRQ_TRIGGER_NONE) || (trigger > GPIO_IRQ_TRIGGER_BOTH) || (cb == 0))
    {
        return RET_INVALID_PARAM;
    }

    /* 所有 GPIO 都通过设备名查找，避免上层依赖具体端口和引脚。 */
    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    /* 默认优先级沿用老工程 EXTI5_9_IRQn 的 6,0 该优先级低于 FreeRTOS max syscall 阈值，可以在 ISR 中调用 FromISR API。 */
    cfg_irq.trigger = trigger;
    cfg_irq.cb = cb;
    cfg_irq.user_ctx = user_ctx;
    cfg_irq.preemption_priority = 6U;
    cfg_irq.sub_priority = 0U;

    /* 通过统一 control 入口让具体驱动完成 EXTI 线、AFIO 和 NVIC 配置。 */
    ret = device_control(dev, DEV_GPIO_CMD_ATTACH_IRQ, &cfg_irq);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 解绑 GPIO 输入中断回调
@para:name GPIO 逻辑设备名
@return: RET_OK 表示成功，
         RET_INVALID_PARAM 表示参数非法，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_gpio_detach_irq(const char *name)
{
    device_t *dev;
    int ret;

    if (name == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    /* 底层驱动会关闭 EXTI line 并清掉保存的回调上下文。 */
    ret = device_control(dev, DEV_GPIO_CMD_DETACH_IRQ, 0);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 打开或关闭 GPIO 中断事件上报
@para:name GPIO 逻辑设备名
@para:enable 0 表示关闭中断，非 0 表示打开中断
@return: RET_OK 表示成功，
         RET_INVALID_PARAM 表示参数非法，
         RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_gpio_irq_enable(const char *name, uint8_t enable)
{
    device_t *dev;
    uint8_t irq_enable;
    int ret;

    if (name == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = device_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    /* control 参数使用 uint8_t，避免不同平台 bool 类型大小不一致。 */
    irq_enable = (enable != 0U) ? 1U : 0U;
    ret = device_control(dev, DEV_GPIO_CMD_ENABLE_IRQ, &irq_enable);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}
