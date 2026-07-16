#include "drv_pwm.h"

#include "device.h"
#include "return_code.h"
#include "system_gd32f10x.h"

/* GD32 PWM 适配层最多可注册的 PWM 设备数量。 */
#define GD32_PWM_DEVICE_MAX        8U

/* PWM 计数频率固定为 1MHz，计数单位为 1us，便于舵机脉宽控制。 */
#define GD32_PWM_COUNTER_HZ        1000000U

/* PWM 占空比最大值，1000 表示 100%。 */
#define GD32_PWM_DUTY_MAX          1000U

/* 单个 GD32 PWM 设备的运行时私有数据。 */
typedef struct
{
    rcu_periph_enum timer_rcu;        /* 定时器外设时钟枚举。 */
    rcu_periph_enum gpio_rcu;         /* PWM 输出 GPIO 时钟枚举。 */
    uint32_t timer_periph;            /* 定时器外设基地址。 */
    uint16_t channel;                 /* 定时器输出通道。 */
    uint32_t gpio_periph;             /* GPIO 外设基地址。 */
    uint32_t gpio_pin;                /* PWM 输出引脚。 */
    uint32_t gpio_mode;               /* PWM 输出引脚模式。 */
    uint32_t gpio_speed;              /* PWM 输出引脚速度。 */
    uint32_t source_clock_hz;          /* 定时器源时钟，0 表示使用 SystemCoreClock。 */
    uint32_t counter_hz;              /* 实际计数频率。 */
    uint32_t period_counts;           /* PWM 周期计数值。 */
    uint32_t pulse_counts;            /* PWM 有效脉宽计数值。 */
    pwm_config_t config;              /* 当前 PWM 配置。 */
} gd32_pwm_dev_t;

/* PWM 设备注册槽，绑定通用设备对象和 GD32 PWM 私有数据。 */
typedef struct
{
    device_t dev;                     /* 注册到设备层的通用设备对象。 */
    gd32_pwm_dev_t pwm;               /* GD32 PWM 私有数据。 */
} gd32_pwm_slot_t;

/* GD32 PWM 静态注册表。 */
static gd32_pwm_slot_t s_pwm_slots[GD32_PWM_DEVICE_MAX];

/* 当前已经注册的 GD32 PWM 设备数量。 */
static uint8_t s_pwm_count;

/*****************************************************************************
@brief: 将 32 位计数值转换为 16 位寄存器值，超出范围时饱和到最大值
@para:value 待转换的 32 位数值
@return: 转换后的 16 位数值
*******************************************************************************/
static uint16_t gd32_pwm_to_u16_saturated(uint32_t value)
{
    return (value > 0xFFFFU) ? 0xFFFFU : (uint16_t)value;
}

static uint8_t gd32_pwm_timer_is_apb2(uint32_t timer_periph)
{
    return ((timer_periph == TIMER0) ||
            (timer_periph == TIMER7) ||
            (timer_periph == TIMER8) ||
            (timer_periph == TIMER9) ||
            (timer_periph == TIMER10)) ? 1U : 0U;
}

static uint32_t gd32_pwm_get_bus_timer_clock(uint32_t timer_periph)
{
    uint32_t pclk;
    uint32_t psc;

    if (gd32_pwm_timer_is_apb2(timer_periph) != 0U)
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
@brief: 获取 PWM 定时器源时钟
@para:pwm PWM 私有数据指针
@return: 定时器源时钟频率，单位 Hz
*******************************************************************************/
static uint32_t gd32_pwm_get_source_clock(const gd32_pwm_dev_t *pwm)
{
    if ((pwm != 0) && (pwm->source_clock_hz != 0U))
    {
        return pwm->source_clock_hz;
    }

    return (pwm != 0) ? gd32_pwm_get_bus_timer_clock(pwm->timer_periph) : SystemCoreClock;
}

/*****************************************************************************
@brief: 根据 PWM 频率计算周期计数值
@para:freq_hz PWM 频率，单位 Hz
@return: PWM 周期计数值
*******************************************************************************/
static uint32_t gd32_pwm_freq_to_period_counts(uint32_t freq_hz)
{
    if (freq_hz == 0U)
    {
        return 0U;
    }

    return GD32_PWM_COUNTER_HZ / freq_hz;
}

/*****************************************************************************
@brief: 根据周期和占空比计算有效脉宽计数值
@para:period_counts PWM 周期计数值
@para:duty_permille PWM 占空比，单位为千分比
@return: PWM 有效脉宽计数值
*******************************************************************************/
static uint32_t gd32_pwm_duty_to_pulse_counts(uint32_t period_counts, uint16_t duty_permille)
{
    return (period_counts * (uint32_t)duty_permille) / GD32_PWM_DUTY_MAX;
}

/*****************************************************************************
@brief: 将当前有效脉宽写入 PWM 输出比较寄存器
@para:pwm PWM 私有数据指针
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_apply_pulse(gd32_pwm_dev_t *pwm)
{
    if ((pwm == 0) || (pwm->period_counts == 0U) || (pwm->pulse_counts > pwm->period_counts))
    {
        return RET_INVALID_PARAM;
    }

    timer_channel_output_pulse_value_config(pwm->timer_periph,
                                            pwm->channel,
                                            gd32_pwm_to_u16_saturated(pwm->pulse_counts));

    return RET_OK;
}

/*****************************************************************************
@brief: 根据当前配置重新配置 PWM 定时器
@para:pwm PWM 私有数据指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_apply_timer(gd32_pwm_dev_t *pwm)
{
    uint32_t source_clock_hz;
    uint32_t prescaler;
    timer_parameter_struct timer_init_para;
    timer_oc_parameter_struct oc_init_para;

    if ((pwm == 0) || (pwm->config.freq_hz == 0U) ||
        (pwm->config.duty_permille > GD32_PWM_DUTY_MAX) ||
        (pwm->config.polarity > PWM_POLARITY_INVERTED))
    {
        return RET_INVALID_PARAM;
    }

    pwm->period_counts = gd32_pwm_freq_to_period_counts(pwm->config.freq_hz);
    if ((pwm->period_counts == 0U) || (pwm->period_counts > 0x10000U))
    {
        return RET_INVALID_PARAM;
    }

    source_clock_hz = gd32_pwm_get_source_clock(pwm);
    if (source_clock_hz < GD32_PWM_COUNTER_HZ)
    {
        return RET_INVALID_PARAM;
    }

    prescaler = (source_clock_hz / GD32_PWM_COUNTER_HZ);
    if ((prescaler == 0U) || (prescaler > 0x10000U))
    {
        return RET_INVALID_PARAM;
    }

    timer_deinit(pwm->timer_periph);

    timer_struct_para_init(&timer_init_para);
    timer_init_para.prescaler = gd32_pwm_to_u16_saturated(prescaler - 1U);
    timer_init_para.period = gd32_pwm_to_u16_saturated(pwm->period_counts - 1U);
    timer_init(pwm->timer_periph, &timer_init_para);
    timer_auto_reload_shadow_enable(pwm->timer_periph);

    timer_channel_output_struct_para_init(&oc_init_para);
    oc_init_para.outputstate = TIMER_CCX_ENABLE;
    oc_init_para.ocpolarity = (pwm->config.polarity == PWM_POLARITY_NORMAL) ? TIMER_OC_POLARITY_HIGH : TIMER_OC_POLARITY_LOW;
    timer_channel_output_config(pwm->timer_periph, pwm->channel, &oc_init_para);
    timer_channel_output_mode_config(pwm->timer_periph, pwm->channel, TIMER_OC_MODE_PWM0);
    timer_channel_output_shadow_config(pwm->timer_periph, pwm->channel, TIMER_OC_SHADOW_ENABLE);
    timer_primary_output_config(pwm->timer_periph, ENABLE);

    pwm->counter_hz = GD32_PWM_COUNTER_HZ;
    pwm->pulse_counts = gd32_pwm_duty_to_pulse_counts(pwm->period_counts, pwm->config.duty_permille);

    return gd32_pwm_apply_pulse(pwm);
}

/*****************************************************************************
@brief: 初始化 GD32 PWM 硬件资源
@para:dev 设备层传入的 PWM 设备对象
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_init(device_t *dev)
{
    gd32_pwm_dev_t *pwm;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    pwm = (gd32_pwm_dev_t *)dev->user_data;

    rcu_periph_clock_enable(pwm->gpio_rcu);
    rcu_periph_clock_enable(pwm->timer_rcu);
    gpio_init(pwm->gpio_periph, pwm->gpio_mode, pwm->gpio_speed, pwm->gpio_pin);

    return gd32_pwm_apply_timer(pwm);
}

/*****************************************************************************
@brief: 打开 GD32 PWM 输出
@para:dev 设备层传入的 PWM 设备对象
@return: RET_OK 表示打开成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_open(device_t *dev)
{
    gd32_pwm_dev_t *pwm;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    pwm = (gd32_pwm_dev_t *)dev->user_data;
    timer_enable(pwm->timer_periph);

    return RET_OK;
}

/*****************************************************************************
@brief: 关闭 GD32 PWM 输出
@para:dev 设备层传入的 PWM 设备对象
@return: RET_OK 表示关闭成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_close(device_t *dev)
{
    gd32_pwm_dev_t *pwm;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    pwm = (gd32_pwm_dev_t *)dev->user_data;
    timer_disable(pwm->timer_periph);

    return RET_OK;
}

/*****************************************************************************
@brief: 更新 GD32 PWM 配置
@para:dev 设备层传入的 PWM 设备对象
@para:cfg 新的 PWM 配置参数
@return: RET_OK 表示更新成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_set_config(device_t *dev, const pwm_config_t *cfg)
{
    gd32_pwm_dev_t *pwm;
    uint8_t was_opened;
    int ret;

    if ((dev == 0) || (dev->user_data == 0) || (cfg == 0))
    {
        return RET_INVALID_PARAM;
    }

    pwm = (gd32_pwm_dev_t *)dev->user_data;
    was_opened = dev->opened;
    pwm->config = *cfg;

    ret = gd32_pwm_apply_timer(pwm);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (was_opened != 0U)
    {
        timer_enable(pwm->timer_periph);
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 按微秒直接设置 PWM 有效脉宽
@para:pwm PWM 私有数据指针
@para:pulse_width_us 目标有效脉宽，单位 us
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_set_pulse_us(gd32_pwm_dev_t *pwm, uint32_t pulse_width_us)
{
    uint32_t pulse_counts;

    if ((pwm == 0) || (pulse_width_us == 0U) || (pwm->counter_hz == 0U))
    {
        return RET_INVALID_PARAM;
    }

    pulse_counts = (uint32_t)(((uint64_t)pulse_width_us * (uint64_t)pwm->counter_hz) / 1000000ULL);
    if ((pulse_counts == 0U) || (pulse_counts > pwm->period_counts))
    {
        return RET_INVALID_PARAM;
    }

    pwm->pulse_counts = pulse_counts;
    pwm->config.duty_permille = (uint16_t)((pulse_counts * GD32_PWM_DUTY_MAX) / pwm->period_counts);

    return gd32_pwm_apply_pulse(pwm);
}

/*****************************************************************************
@brief: GD32 PWM 设备控制接口
@para:dev 设备层传入的 PWM 设备对象
@para:cmd PWM 控制命令
@para:arg 控制命令参数
@return: RET_OK 表示控制成功，其他返回值表示失败
*******************************************************************************/
static int gd32_pwm_control(device_t *dev, int cmd, void *arg)
{
    gd32_pwm_dev_t *pwm;
    pwm_config_t cfg;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    pwm = (gd32_pwm_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_PWM_CMD_CONFIG:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        return gd32_pwm_set_config(dev, (const pwm_config_t *)arg);

    case DEV_PWM_CMD_SET_FREQ:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        cfg = pwm->config;
        cfg.freq_hz = *(const uint32_t *)arg;
        return gd32_pwm_set_config(dev, &cfg);

    case DEV_PWM_CMD_SET_DUTY:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        cfg = pwm->config;
        cfg.duty_permille = *(const uint16_t *)arg;
        return gd32_pwm_set_config(dev, &cfg);

    case DEV_PWM_CMD_GET_DUTY:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        *(uint16_t *)arg = pwm->config.duty_permille;
        return RET_OK;

    case DEV_PWM_CMD_SET_PULSE_WIDTH_US:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        return gd32_pwm_set_pulse_us(pwm, *(const uint32_t *)arg);

    default:
        return RET_NOT_SUPPORTED;
    }
}

/* GD32 PWM 设备操作函数表。 */
static const device_ops_t s_gd32_pwm_ops =
{
    gd32_pwm_init,
    gd32_pwm_open,
    gd32_pwm_close,
    0,
    0,
    gd32_pwm_control,
};

/*****************************************************************************
@brief: 注册一个 GD32 PWM 设备
@para:cfg PWM 硬件资源配置
@return: RET_OK 表示注册成功，
         RET_INVALID_PARAM 表示参数无效，
         RET_NO_RESOURCE 表示注册槽已满
*******************************************************************************/
int gd32_pwm_register(const gd32_pwm_cfg_t *cfg)
{
    gd32_pwm_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->default_config.freq_hz == 0U) ||
        (cfg->default_config.duty_permille > GD32_PWM_DUTY_MAX) ||
        (cfg->default_config.polarity > PWM_POLARITY_INVERTED))
    {
        return RET_INVALID_PARAM;
    }

    if (s_pwm_count >= GD32_PWM_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    slot = &s_pwm_slots[s_pwm_count];

    //复制PWM的私有数据
    slot->pwm.timer_rcu = cfg->timer_rcu;
    slot->pwm.gpio_rcu = cfg->gpio_rcu;
    slot->pwm.timer_periph = cfg->timer_periph;
    slot->pwm.channel = cfg->channel;
    slot->pwm.gpio_periph = cfg->gpio_periph;
    slot->pwm.gpio_pin = cfg->gpio_pin;
    slot->pwm.gpio_mode = cfg->gpio_mode;
    slot->pwm.gpio_speed = cfg->gpio_speed;
    slot->pwm.source_clock_hz = cfg->source_clock_hz;
    slot->pwm.counter_hz = 0U;
    slot->pwm.period_counts = 0U;
    slot->pwm.pulse_counts = 0U;
    slot->pwm.config = cfg->default_config;

    //通用数据
    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_PWM;
    slot->dev.ops = &s_gd32_pwm_ops;
    slot->dev.user_data = &slot->pwm;
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_pwm_count++;
    }

    return ret;
}
