#include "drv_timer.h"

#include "device.h"
#include "return_code.h"
#include "system_gd32f10x.h"

/* GD32 TIMER 适配层最多可注册的周期定时器设备数量。 */
#define GD32_TIMER_DEVICE_MAX    4U

/* 周期定时器计数频率固定为 1MHz，计数单位为 1us，便于遥控器脉宽解码。 */
#define GD32_TIMER_COUNTER_HZ    1000000U

/* 单个 GD32 周期定时器设备的运行时私有数据。 */
typedef struct
{
    rcu_periph_enum timer_rcu;             /* 定时器外设时钟枚举。 */
    uint32_t timer_periph;                 /* 定时器外设基地址。 */
    IRQn_Type irqn;                        /* 定时器更新中断号。 */
    uint32_t source_clock_hz;              /* 定时器源时钟，0 表示自动计算。 */
    uint8_t preemption_priority;           /* NVIC 抢占优先级。 */
    uint8_t sub_priority;                  /* NVIC 子优先级。 */
    timer_config_t config;                 /* 当前定时器周期配置。 */
    timer_period_elapsed_cb_t cb;          /* 周期到达回调函数。 */
    void *user_ctx;                        /* 回调用户上下文。 */
} gd32_timer_dev_t;

/* TIMER 设备注册槽，绑定通用设备对象和 GD32 TIMER 私有数据。 */
typedef struct
{
    device_t dev;                          /* 注册到设备层的通用设备对象。 */
    gd32_timer_dev_t timer;                /* GD32 TIMER 私有数据。 */
} gd32_timer_slot_t;

/* GD32 TIMER 静态注册表。 */
static gd32_timer_slot_t s_timer_slots[GD32_TIMER_DEVICE_MAX];

/* 当前已经注册的 GD32 TIMER 设备数量。 */
static uint8_t s_timer_count;

/*****************************************************************************
@brief: 将 32 位计数值转换为 16 位寄存器值，超出范围时饱和到最大值
@para:value 待转换的 32 位数值
@return: 转换后的 16 位数值
*******************************************************************************/
static uint16_t gd32_timer_to_u16_saturated(uint32_t value)
{
    return (value > 0xFFFFU) ? 0xFFFFU : (uint16_t)value;
}

/*****************************************************************************
@brief: 判断定时器是否挂载在 APB2 总线上
@para:timer_periph 定时器外设基地址
@return: 非 0 表示 APB2，0 表示 APB1
*******************************************************************************/
static uint8_t gd32_timer_is_apb2(uint32_t timer_periph)
{
    return ((timer_periph == TIMER0) ||
            (timer_periph == TIMER7) ||
            (timer_periph == TIMER8) ||
            (timer_periph == TIMER9) ||
            (timer_periph == TIMER10)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 获取定时器所在 APB 总线提供给 TIMER 的实际时钟
@para:timer_periph 定时器外设基地址
@return: TIMER 源时钟频率，单位 Hz
*******************************************************************************/
static uint32_t gd32_timer_get_bus_clock(uint32_t timer_periph)
{
    uint32_t pclk;
    uint32_t psc;

    if (gd32_timer_is_apb2(timer_periph) != 0U)
    {
        pclk = rcu_clock_freq_get(CK_APB2);
        psc = RCU_CFG0 & RCU_CFG0_APB2PSC;
    }
    else
    {
        pclk = rcu_clock_freq_get(CK_APB1);
        psc = RCU_CFG0 & RCU_CFG0_APB1PSC;
    }

    return (psc == 0U) ? pclk : (pclk * 2U);
}

/*****************************************************************************
@brief: 获取周期定时器源时钟
@para:timer TIMER 私有数据指针
@return: 定时器源时钟频率，单位 Hz
*******************************************************************************/
static uint32_t gd32_timer_get_source_clock(const gd32_timer_dev_t *timer)
{
    if ((timer != 0) && (timer->source_clock_hz != 0U))
    {
        return timer->source_clock_hz;
    }

    return (timer != 0) ? gd32_timer_get_bus_clock(timer->timer_periph) : SystemCoreClock;
}

/*****************************************************************************
@brief: 根据当前配置重新配置 GD32 周期定时器
@para:timer TIMER 私有数据指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_apply_config(gd32_timer_dev_t *timer)
{
    uint32_t source_clock_hz;
    uint32_t prescaler;
    uint32_t period_counts;
    timer_parameter_struct timer_init_para;

    if ((timer == 0) || (timer->config.period_us == 0U))
    {
        return RET_INVALID_PARAM;
    }

    period_counts = timer->config.period_us;
    if ((period_counts == 0U) || (period_counts > 0x10000U))
    {
        return RET_INVALID_PARAM;
    }

    source_clock_hz = gd32_timer_get_source_clock(timer);
    if (source_clock_hz < GD32_TIMER_COUNTER_HZ)
    {
        return RET_INVALID_PARAM;
    }

    prescaler = source_clock_hz / GD32_TIMER_COUNTER_HZ;
    if ((prescaler == 0U) || (prescaler > 0x10000U))
    {
        return RET_INVALID_PARAM;
    }

    timer_disable(timer->timer_periph);
    timer_deinit(timer->timer_periph);

    timer_struct_para_init(&timer_init_para);
    timer_init_para.prescaler = gd32_timer_to_u16_saturated(prescaler - 1U);
    timer_init_para.period = gd32_timer_to_u16_saturated(period_counts - 1U);
    timer_init(timer->timer_periph, &timer_init_para);
    timer_auto_reload_shadow_enable(timer->timer_periph);
    timer_interrupt_flag_clear(timer->timer_periph, TIMER_INT_FLAG_UP);
    timer_interrupt_enable(timer->timer_periph, TIMER_INT_UP);

    return RET_OK;
}

/*****************************************************************************
@brief: 初始化 GD32 周期定时器硬件资源
@para:dev 设备层传入的 TIMER 设备对象
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_init(device_t *dev)
{
    gd32_timer_dev_t *timer;
    int ret;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    timer = (gd32_timer_dev_t *)dev->user_data;

    rcu_periph_clock_enable(timer->timer_rcu);

    ret = gd32_timer_apply_config(timer);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    nvic_irq_enable(timer->irqn, timer->preemption_priority, timer->sub_priority);

    return RET_OK;
}

/*****************************************************************************
@brief: 打开 GD32 周期定时器
@para:dev 设备层传入的 TIMER 设备对象
@return: RET_OK 表示打开成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_open(device_t *dev)
{
    gd32_timer_dev_t *timer;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    timer = (gd32_timer_dev_t *)dev->user_data;
    timer_interrupt_flag_clear(timer->timer_periph, TIMER_INT_FLAG_UP);
    timer_enable(timer->timer_periph);

    return RET_OK;
}

/*****************************************************************************
@brief: 关闭 GD32 周期定时器
@para:dev 设备层传入的 TIMER 设备对象
@return: RET_OK 表示关闭成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_close(device_t *dev)
{
    gd32_timer_dev_t *timer;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    timer = (gd32_timer_dev_t *)dev->user_data;
    timer_disable(timer->timer_periph);
    timer_interrupt_flag_clear(timer->timer_periph, TIMER_INT_FLAG_UP);

    return RET_OK;
}

/*****************************************************************************
@brief: 更新 GD32 周期定时器配置
@para:dev 设备层传入的 TIMER 设备对象
@para:cfg 新的周期配置
@return: RET_OK 表示更新成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_set_config(device_t *dev, const timer_config_t *cfg)
{
    gd32_timer_dev_t *timer;
    uint8_t was_opened;
    int ret;

    if ((dev == 0) || (dev->user_data == 0) || (cfg == 0) || (cfg->period_us == 0U))
    {
        return RET_INVALID_PARAM;
    }

    timer = (gd32_timer_dev_t *)dev->user_data;
    was_opened = dev->opened;
    timer->config = *cfg;

    ret = gd32_timer_apply_config(timer);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (was_opened != 0U)
    {
        timer_enable(timer->timer_periph);
    }

    return RET_OK;
}

/*****************************************************************************
@brief: GD32 周期定时器设备控制接口
@para:dev 设备层传入的 TIMER 设备对象
@para:cmd TIMER 控制命令
@para:arg 控制命令参数
@return: RET_OK 表示控制成功，其他返回值表示失败
*******************************************************************************/
static int gd32_timer_control(device_t *dev, int cmd, void *arg)
{
    gd32_timer_dev_t *timer;
    const timer_callback_config_t *cb_cfg;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    timer = (gd32_timer_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_TIMER_CMD_CONFIG:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        return gd32_timer_set_config(dev, (const timer_config_t *)arg);

    case DEV_TIMER_CMD_ATTACH_CALLBACK:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        cb_cfg = (const timer_callback_config_t *)arg;
        if (cb_cfg->cb == 0)
        {
            return RET_INVALID_PARAM;
        }
        timer->cb = cb_cfg->cb;
        timer->user_ctx = cb_cfg->user_ctx;
        return RET_OK;

    case DEV_TIMER_CMD_DETACH_CALLBACK:
        timer->cb = 0;
        timer->user_ctx = 0;
        return RET_OK;

    default:
        return RET_NOT_SUPPORTED;
    }
}

/* GD32 TIMER 设备操作函数表。 */
static const device_ops_t s_gd32_timer_ops =
{
    gd32_timer_init,
    gd32_timer_open,
    gd32_timer_close,
    0,
    0,
    gd32_timer_control,
};

/*****************************************************************************
@brief: 注册一个 GD32 周期定时器设备
@para:cfg 定时器硬件资源配置
@return: RET_OK 表示注册成功，RET_INVALID_PARAM 表示参数无效，RET_NO_RESOURCE 表示注册槽已满
*******************************************************************************/
int gd32_timer_register(const gd32_timer_cfg_t *cfg)
{
    gd32_timer_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->default_config.period_us == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (s_timer_count >= GD32_TIMER_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    slot = &s_timer_slots[s_timer_count];

    slot->timer.timer_rcu = cfg->timer_rcu;
    slot->timer.timer_periph = cfg->timer_periph;
    slot->timer.irqn = cfg->irqn;
    slot->timer.source_clock_hz = cfg->source_clock_hz;
    slot->timer.preemption_priority = cfg->preemption_priority;
    slot->timer.sub_priority = cfg->sub_priority;
    slot->timer.config = cfg->default_config;
    slot->timer.cb = 0;
    slot->timer.user_ctx = 0;

    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_TIMER;
    slot->dev.ops = &s_gd32_timer_ops;
    slot->dev.user_data = &slot->timer;
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_timer_count++;
    }

    return ret;
}

/*****************************************************************************
@brief: GD32 TIMER 更新中断统一分发函数
@para:timer_periph 触发中断的 TIMER 外设基地址
@return:
@note: ISR 中只清标志并调用已绑定的轻量回调，不写业务策略。
*******************************************************************************/
static void gd32_timer_irq_dispatch(uint32_t timer_periph)
{
    gd32_timer_slot_t *slot;
    gd32_timer_dev_t *timer;
    uint8_t i;

    if (timer_interrupt_flag_get(timer_periph, TIMER_INT_FLAG_UP) == RESET)
    {
        return;
    }

    timer_interrupt_flag_clear(timer_periph, TIMER_INT_FLAG_UP);

    for (i = 0U; i < s_timer_count; i++)
    {
        slot = &s_timer_slots[i];
        timer = &slot->timer;

        if ((timer->timer_periph == timer_periph) && (timer->cb != 0))
        {
            timer->cb(slot->dev.name, timer->user_ctx);
        }
    }
}

/*****************************************************************************
@brief: TIMER4 全局中断入口
@para:
@return:
*******************************************************************************/
void TIMER4_IRQHandler(void)
{
    gd32_timer_irq_dispatch(TIMER4);
}

/*****************************************************************************
@brief: TIMER5 全局中断入口
@para:
@return:
*******************************************************************************/
void TIMER5_IRQHandler(void)
{
    gd32_timer_irq_dispatch(TIMER5);
}

/*****************************************************************************
@brief: TIMER6 全局中断入口
@para:
@return:
*******************************************************************************/
void TIMER6_IRQHandler(void)
{
    gd32_timer_irq_dispatch(TIMER6);
}
