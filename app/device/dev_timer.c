#include "dev_timer.h"

#include "device.h"
#include "return_code.h"

/*****************************************************************************
@brief: 按名称查找 TIMER 设备
@para:name TIMER 设备名称
@return: 查找到 TIMER 设备时返回设备指针，否则返回空指针
*******************************************************************************/
static device_t *dev_timer_find(const char *name)
{
    device_t *dev;

    if (name == 0)
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_TIMER))
    {
        return 0;
    }

    return dev;
}

/*****************************************************************************
@brief: 配置周期定时器参数
@para:name 定时器设备名称
@para:cfg 定时器周期配置
@return: RET_OK 表示配置成功，RET_INVALID_PARAM 表示参数无效，RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_timer_config(const char *name, const timer_config_t *cfg)
{
    device_t *dev;
    int ret;

    if ((cfg == 0) || (cfg->period_us == 0U))
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_timer_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_TIMER_CMD_CONFIG, (void *)cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 绑定周期定时器中断回调
@para:name 定时器设备名称
@para:cb 周期到达回调函数，运行在 ISR 上下文中
@para:user_ctx 用户上下文指针，回调时原样传回
@return: RET_OK 表示绑定成功，RET_INVALID_PARAM 表示参数无效，RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_timer_attach_callback(const char *name, timer_period_elapsed_cb_t cb, void *user_ctx)
{
    device_t *dev;
    timer_callback_config_t cb_cfg;
    int ret;

    if (cb == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_timer_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    cb_cfg.cb = cb;
    cb_cfg.user_ctx = user_ctx;

    ret = device_control(dev, DEV_TIMER_CMD_ATTACH_CALLBACK, &cb_cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 解绑周期定时器中断回调
@para:name 定时器设备名称
@return: RET_OK 表示解绑成功，RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_timer_detach_callback(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_timer_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_TIMER_CMD_DETACH_CALLBACK, 0);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 启动周期定时器
@para:name 定时器设备名称
@return: RET_OK 表示启动成功，RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_timer_start(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_timer_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_open(dev);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 停止周期定时器
@para:name 定时器设备名称
@return: RET_OK 表示停止成功，RET_NOT_FOUND 表示设备不存在
*******************************************************************************/
int dev_timer_stop(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_timer_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_close(dev);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}
