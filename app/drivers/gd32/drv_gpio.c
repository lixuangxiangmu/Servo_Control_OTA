#include "drv_gpio.h"

#include "device.h"
#include "dev_gpio.h"
#include "return_code.h"

/* Maximum number of GPIO devices that can be registered in the GD32 GPIO
   adaptation layer. */
#define GD32_GPIO_DEVICE_MAX 24U

/* 单个 GD32 GPIO 设备的硬件资源描述。
   前半部分描述普通 GPIO 读写所需的端口、引脚和模式；
   后半部分是可选的 EXTI 中断运行时状态，仅在 dev_gpio_attach_irq() 后启用。 */
typedef struct
{
    rcu_periph_enum rcu;           /* GPIO 外设时钟枚举，例如 RCU_GPIOB。 */
    uint32_t gpio_periph;          /* GPIO 端口基地址，例如 GPIOB。 */
    uint32_t pin;                  /* GPIO 引脚掩码，例如 GPIO_PIN_8。 */
    uint32_t mode;                 /* GD32 原生 GPIO 模式，例如 GPIO_MODE_IN_FLOATING。 */
    uint32_t speed;                /* GD32 原生 GPIO 速度配置。 */
    uint8_t default_level;         /* 输出模式初始化前预置电平，输入模式下仅作占位。 */
    uint8_t exti_attached;         /* 是否已经完成 EXTI 绑定，防止 enable 时误操作未配置的 line。 */
    uint8_t irq_enabled;           /* 当前是否允许向上层派发 GPIO 中断事件。 */
    exti_line_enum exti_line;      /* 当前 GPIO 引脚对应的 EXTI line。 */
    gpio_irq_trigger_t irq_trigger;/* 抽象层配置的触发边沿，用于 ISR 中还原事件类型。 */
    gpio_irq_cb_t irq_cb;          /* 上层注册的中断回调。 */
    void *irq_user_ctx;            /* 上层注册的回调上下文，驱动层只保存和透传。 */
} gd32_gpio_dev_t;

/* GPIO device registration slot, binding a device object with GD32 GPIO private data. */
typedef struct
{
    device_t dev;          /* Generic device object registered with the device layer. */
    gd32_gpio_dev_t gpio;  /* GD32 GPIO private data. */
} gd32_gpio_slot_t;

/* Static registry for GD32 GPIO devices. */
static gd32_gpio_slot_t s_gpio_slots[GD32_GPIO_DEVICE_MAX];

/* Current count of registered GD32 GPIO devices. */
static uint8_t s_gpio_count;

static uint8_t gd32_gpio_mode_is_output(uint32_t mode)
{
    return ((mode == GPIO_MODE_OUT_PP) || (mode == GPIO_MODE_OUT_OD)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 将 GD32 GPIO 端口基地址转换为 AFIO/EXTI 端口源编号
@para:gpio_periph GPIO 端口基地址，例如 GPIOA/GPIOB
@para:port_source 输出端口源编号，例如 GPIO_PORT_SOURCE_GPIOB
@return: RET_OK 表示转换成功，其他返回值表示端口不支持或参数非法
@note: GD32 的 gpio_exti_source_select() 不直接接收 GPIOB 这类基地址，
       所以需要在驱动层做一次映射。
*******************************************************************************/
static int gd32_gpio_get_port_source(uint32_t gpio_periph, uint8_t *port_source)
{
    if (port_source == 0)
    {
        return RET_INVALID_PARAM;
    }

    switch (gpio_periph)
    {
    case GPIOA:
        *port_source = GPIO_PORT_SOURCE_GPIOA;
        break;

    case GPIOB:
        *port_source = GPIO_PORT_SOURCE_GPIOB;
        break;

    case GPIOC:
        *port_source = GPIO_PORT_SOURCE_GPIOC;
        break;

    case GPIOD:
        *port_source = GPIO_PORT_SOURCE_GPIOD;
        break;

    case GPIOE:
        *port_source = GPIO_PORT_SOURCE_GPIOE;
        break;

    case GPIOF:
        *port_source = GPIO_PORT_SOURCE_GPIOF;
        break;

    case GPIOG:
        *port_source = GPIO_PORT_SOURCE_GPIOG;
        break;

    default:
        return RET_NOT_SUPPORTED;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 将单个 GPIO_PIN_x 转换为 EXTI line、pin source 和 NVIC IRQn
@para:pin GPIO 引脚掩码，当前只支持单 bit 引脚
@para:pin_source 输出 AFIO/EXTI 使用的 pin source
@para:exti_line 输出 EXTI line
@para:irqn 输出该 EXTI line 所属的 NVIC 中断号
@return: RET_OK 表示转换成功，其他返回值表示参数非法或引脚不支持
@note: GD32F10x 中 EXTI0~4 独立中断，EXTI5~9 共用 EXTI5_9_IRQn，
       EXTI10~15 共用 EXTI10_15_IRQn。
*******************************************************************************/
static int gd32_gpio_pin_to_exti(uint32_t pin,
                                 uint8_t *pin_source,
                                 exti_line_enum *exti_line,
                                 IRQn_Type *irqn)
{
    uint8_t source;
    exti_line_enum line;
    IRQn_Type irq;

    if ((pin_source == 0) || (exti_line == 0) || (irqn == 0))
    {
        return RET_INVALID_PARAM;
    }

    switch (pin)
    {
    case GPIO_PIN_0:
        source = GPIO_PIN_SOURCE_0;
        line = EXTI_0;
        irq = EXTI0_IRQn;
        break;

    case GPIO_PIN_1:
        source = GPIO_PIN_SOURCE_1;
        line = EXTI_1;
        irq = EXTI1_IRQn;
        break;

    case GPIO_PIN_2:
        source = GPIO_PIN_SOURCE_2;
        line = EXTI_2;
        irq = EXTI2_IRQn;
        break;

    case GPIO_PIN_3:
        source = GPIO_PIN_SOURCE_3;
        line = EXTI_3;
        irq = EXTI3_IRQn;
        break;

    case GPIO_PIN_4:
        source = GPIO_PIN_SOURCE_4;
        line = EXTI_4;
        irq = EXTI4_IRQn;
        break;

    case GPIO_PIN_5:
        source = GPIO_PIN_SOURCE_5;
        line = EXTI_5;
        irq = EXTI5_9_IRQn;
        break;

    case GPIO_PIN_6:
        source = GPIO_PIN_SOURCE_6;
        line = EXTI_6;
        irq = EXTI5_9_IRQn;
        break;

    case GPIO_PIN_7:
        source = GPIO_PIN_SOURCE_7;
        line = EXTI_7;
        irq = EXTI5_9_IRQn;
        break;

    case GPIO_PIN_8:
        source = GPIO_PIN_SOURCE_8;
        line = EXTI_8;
        irq = EXTI5_9_IRQn;
        break;

    case GPIO_PIN_9:
        source = GPIO_PIN_SOURCE_9;
        line = EXTI_9;
        irq = EXTI5_9_IRQn;
        break;

    case GPIO_PIN_10:
        source = GPIO_PIN_SOURCE_10;
        line = EXTI_10;
        irq = EXTI10_15_IRQn;
        break;

    case GPIO_PIN_11:
        source = GPIO_PIN_SOURCE_11;
        line = EXTI_11;
        irq = EXTI10_15_IRQn;
        break;

    case GPIO_PIN_12:
        source = GPIO_PIN_SOURCE_12;
        line = EXTI_12;
        irq = EXTI10_15_IRQn;
        break;

    case GPIO_PIN_13:
        source = GPIO_PIN_SOURCE_13;
        line = EXTI_13;
        irq = EXTI10_15_IRQn;
        break;

    case GPIO_PIN_14:
        source = GPIO_PIN_SOURCE_14;
        line = EXTI_14;
        irq = EXTI10_15_IRQn;
        break;

    case GPIO_PIN_15:
        source = GPIO_PIN_SOURCE_15;
        line = EXTI_15;
        irq = EXTI10_15_IRQn;
        break;

    default:
        return RET_NOT_SUPPORTED;
    }

    *pin_source = source;
    *exti_line = line;
    *irqn = irq;

    return RET_OK;
}

/*****************************************************************************
@brief: 将 device 层的 GPIO 触发枚举转换为 GD32 EXTI 触发枚举
@para:trigger device 层触发方式
@para:hw_trigger 输出 GD32 标准外设库触发方式
@return: RET_OK 表示转换成功，其他返回值表示参数非法
*******************************************************************************/
static int gd32_gpio_trigger_to_hw(gpio_irq_trigger_t trigger, exti_trig_type_enum *hw_trigger)
{
    if (hw_trigger == 0)
    {
        return RET_INVALID_PARAM;
    }

    switch (trigger)
    {
    case GPIO_IRQ_TRIGGER_RISING:
        *hw_trigger = EXTI_TRIG_RISING;
        break;

    case GPIO_IRQ_TRIGGER_FALLING:
        *hw_trigger = EXTI_TRIG_FALLING;
        break;

    case GPIO_IRQ_TRIGGER_BOTH:
        *hw_trigger = EXTI_TRIG_BOTH;
        break;

    default:
        return RET_INVALID_PARAM;
    }

    return RET_OK;
}

static int gd32_gpio_mode_to_hw(gpio_mode_t mode, uint32_t *hw_mode)
{
    if (hw_mode == 0)
    {
        return RET_INVALID_PARAM;
    }

    switch (mode)
    {
    case GPIO_DEV_MODE_OUTPUT:
        *hw_mode = GPIO_MODE_OUT_PP;
        break;

    case GPIO_DEV_MODE_INPUT:
        *hw_mode = GPIO_MODE_IN_FLOATING;
        break;

    case GPIO_DEV_MODE_INPUT_PULLUP:
        *hw_mode = GPIO_MODE_IPU;
        break;

    case GPIO_DEV_MODE_INPUT_PULLDOWN:
        *hw_mode = GPIO_MODE_IPD;
        break;

    case GPIO_DEV_MODE_OUTPUT_OD:
        *hw_mode = GPIO_MODE_OUT_OD;
        break;

    default:
        return RET_INVALID_PARAM;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: Initialize a GD32 GPIO hardware resource
@para:dev Pointer to the device object
@return: RET_OK indicates successful initialization,
         other return values indicate failure
*******************************************************************************/
static int gd32_gpio_init(device_t *dev)
{
    gd32_gpio_dev_t *gpio;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    gpio = (gd32_gpio_dev_t *)dev->user_data;   //取出该gpio的私有数据

    rcu_periph_clock_enable(gpio->rcu);     //开时钟

    //如果是输出模式则将引脚设置为默认电平
    if (gd32_gpio_mode_is_output(gpio->mode) != 0U)
    {
        gpio_bit_write(gpio->gpio_periph, gpio->pin, (gpio->default_level != 0U) ? SET : RESET);
    }

    gpio_init(gpio->gpio_periph, gpio->mode, gpio->speed, gpio->pin);

    return RET_OK;
}

/*****************************************************************************
@brief: Write a logic level to a GD32 GPIO
@para:dev Pointer to the device object
@para:buf Data buffer to be written, buf[0] is the target level
@para:len Length of data to be written
@return: A positive value indicates the number of bytes written,
         other return values indicate failure
*******************************************************************************/
static int gd32_gpio_write(device_t *dev, const uint8_t *buf, uint32_t len)
{
    gd32_gpio_dev_t *gpio;

    if ((dev == 0) || (dev->user_data == 0) || (buf == 0) || (len < 1U))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    gpio = (gd32_gpio_dev_t *)dev->user_data;
    if (gd32_gpio_mode_is_output(gpio->mode) == 0U)
    {
        return RET_INVALID_STATE;
    }

    gpio_bit_write(gpio->gpio_periph, gpio->pin, (buf[0] != 0U) ? SET : RESET);

    return RET_SUCCESS_VALUE(1U);
}

/*****************************************************************************
@brief: Read the current logic level of a GD32 GPIO
@para:dev Pointer to the device object
@para:buf Data receive buffer, buf[0] stores the current level
@para:len Expected read length
@return: A positive value indicates the number of bytes read,
         other return values indicate failure
*******************************************************************************/
static int gd32_gpio_read(device_t *dev, uint8_t *buf, uint32_t len)
{
    gd32_gpio_dev_t *gpio;

    if ((dev == 0) || (dev->user_data == 0) || (buf == 0) || (len < 1U))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    gpio = (gd32_gpio_dev_t *)dev->user_data;
    buf[0] = (gpio_input_bit_get(gpio->gpio_periph, gpio->pin) == SET) ? 1U : 0U;

    return RET_SUCCESS_VALUE(1U);
}

/*****************************************************************************
@brief: Control a GD32 GPIO device
@para:dev Pointer to the device object
@para:cmd Control command
@para:arg Pointer to command argument
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
static int gd32_gpio_control(device_t *dev, int cmd, void *arg)
{
    gd32_gpio_dev_t *gpio_dev;
    const gpio_irq_config_t *irq_cfg;
    uint8_t port_source;
    uint8_t pin_source;
    uint8_t irq_enable;
    uint32_t hw_mode;
    exti_line_enum exti_line;
    exti_trig_type_enum hw_trigger;
    IRQn_Type irqn;
    int ret;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    gpio_dev = (gd32_gpio_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_GPIO_CMD_SET_MODE:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        ret = gd32_gpio_mode_to_hw(*(const gpio_mode_t *)arg, &hw_mode);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        gpio_init(gpio_dev->gpio_periph, hw_mode, gpio_dev->speed, gpio_dev->pin);
        gpio_dev->mode = hw_mode;
        return RET_OK;

    case DEV_GPIO_CMD_ATTACH_IRQ:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        irq_cfg = (const gpio_irq_config_t *)arg;
        /* attach 必须一次性给出触发边沿和回调函数，否则 ISR 无法正确派发事件。 */
        if ((irq_cfg->cb == 0) || (irq_cfg->trigger == GPIO_IRQ_TRIGGER_NONE) || (irq_cfg->trigger > GPIO_IRQ_TRIGGER_BOTH))
        {
            return RET_INVALID_PARAM;
        }

        /* 将板级注册的 GPIO 资源转换成 EXTI 需要的端口源、引脚源和中断号。 */
        ret = gd32_gpio_get_port_source(gpio_dev->gpio_periph, &port_source);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = gd32_gpio_pin_to_exti(gpio_dev->pin, &pin_source, &exti_line, &irqn);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = gd32_gpio_trigger_to_hw(irq_cfg->trigger, &hw_trigger);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        /* AFIO 时钟是 EXTI 引脚复用选择所必需的，普通 GPIO 初始化不会自动打开它。 */
        rcu_periph_clock_enable(RCU_AF);
        gpio_exti_source_select(port_source, pin_source);
        exti_init(exti_line, EXTI_INTERRUPT, hw_trigger);
        exti_interrupt_flag_clear(exti_line);
        nvic_irq_enable(irqn, irq_cfg->preemption_priority, irq_cfg->sub_priority);

        /* 保存运行时信息，真正的 ISR 会根据 exti_line 在注册表中找到这个 GPIO。 */
        gpio_dev->exti_line = exti_line;
        gpio_dev->irq_trigger = irq_cfg->trigger;
        gpio_dev->irq_cb = irq_cfg->cb;
        gpio_dev->irq_user_ctx = irq_cfg->user_ctx;
        gpio_dev->exti_attached = 1U;
        gpio_dev->irq_enabled = 1U;
        return RET_OK;

    case DEV_GPIO_CMD_DETACH_IRQ:
        if (gpio_dev->exti_attached == 0U)
        {
            return RET_OK;
        }

        /* 先关闭硬件中断，再清除回调，避免解绑过程中仍然派发到无效上下文。 */
        exti_interrupt_disable(gpio_dev->exti_line);
        exti_interrupt_flag_clear(gpio_dev->exti_line);
        gpio_dev->irq_cb = 0;
        gpio_dev->irq_user_ctx = 0;
        gpio_dev->irq_enabled = 0U;
        gpio_dev->exti_attached = 0U;
        return RET_OK;

    case DEV_GPIO_CMD_ENABLE_IRQ:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }

        if (gpio_dev->exti_attached == 0U)
        {
            return RET_INVALID_STATE;
        }

        /* enable/disable 只控制 EXTI line 的中断屏蔽位，不改变端口映射和触发边沿。 */
        irq_enable = (*(const uint8_t *)arg != 0U) ? 1U : 0U;
        if (irq_enable != 0U)
        {
            exti_interrupt_flag_clear(gpio_dev->exti_line);
            exti_interrupt_enable(gpio_dev->exti_line);
        }
        else
        {
            exti_interrupt_disable(gpio_dev->exti_line);
        }

        gpio_dev->irq_enabled = irq_enable;
        return RET_OK;

    default:
        return RET_NOT_SUPPORTED;
    }
}

/* GD32 GPIO device operation function table. */
static const device_ops_t s_gd32_gpio_ops =
{
    gd32_gpio_init,
    0,
    0,
    gd32_gpio_read,
    gd32_gpio_write,
    gd32_gpio_control,
};

/*****************************************************************************
@brief: Register a GD32 push-pull output GPIO device
@para:name Device name registered with the device layer
@para:rcu GPIO peripheral clock enumeration
@para:gpio_periph GPIO peripheral base address
@para:pin GPIO pin mask
@para:default_level Default output level, 0 for low level, non-zero for high level
@return: RET_OK indicates successful registration,
         RET_INVALID_PARAM indicates an invalid parameter,
         RET_NO_RESOURCE indicates the registration slots are full
*******************************************************************************/
int gd32_gpio_register(const gd32_gpio_cfg_t *cfg)
{
    gd32_gpio_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (s_gpio_count >= GD32_GPIO_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    slot = &s_gpio_slots[s_gpio_count];

    slot->gpio.rcu = cfg->rcu;
    slot->gpio.gpio_periph = cfg->gpio_periph;
    slot->gpio.pin = cfg->pin;
    slot->gpio.mode = cfg->mode;
    slot->gpio.speed = cfg->speed;
    slot->gpio.default_level = cfg->default_level;

    //中断相关配置
    slot->gpio.exti_attached = 0U;
    slot->gpio.irq_enabled = 0U;
    slot->gpio.exti_line = EXTI_0;
    slot->gpio.irq_trigger = GPIO_IRQ_TRIGGER_NONE;
    slot->gpio.irq_cb = 0;
    slot->gpio.irq_user_ctx = 0;

    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_GPIO;
    slot->dev.ops = &s_gd32_gpio_ops;
    //这一句很关键，user_data指向私有对象,之后只要拿到device_t *dev，就能通过dev->user_data找到它对应的GPIO硬件信息
    slot->dev.user_data = &slot->gpio;          
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);  //只传入dev，后面gpio相关配置只需通过user_data来还原
    if (RET_IS_OK(ret))
    {
        s_gpio_count++;
    }

    return ret;
}


/*****************************************************************************
@brief: GD32 GPIO EXTI 统一分发函数
@para:exti_line 当前需要处理的 EXTI line
@return:
@note: 多个 GPIO 会注册到统一的 s_gpio_slots[] 中。ISR 只知道 EXTI line，
       因此这里通过 line 反查对应 GPIO，并调用上层注册的 gpio_irq_cb_t。
       中断里只做清标志和事件分发，不写产品业务逻辑。
*******************************************************************************/
static void gd32_gpio_irq_dispatch(exti_line_enum exti_line)
{
    gd32_gpio_slot_t *slot;
    gd32_gpio_dev_t *gpio;
    gpio_irq_event_t event;
    uint8_t i;

    if (exti_interrupt_flag_get(exti_line) == RESET)
    {
        return;
    }

    /* 先清中断标志，避免回调处理时间稍长时重复进入同一个 pending 事件。 */
    exti_interrupt_flag_clear(exti_line);

    for (i = 0U; i < s_gpio_count; i++)
    {
        slot = &s_gpio_slots[i];
        gpio = &slot->gpio;

        if ((gpio->exti_attached != 0U) && (gpio->irq_enabled != 0U) &&
            (gpio->exti_line == exti_line) && (gpio->irq_cb != 0))
        {
            /* 单边沿触发时，事件类型由配置决定，比在中断里再次读取本引脚电平更稳定。
               双边沿触发时才通过当前电平推断本次事件是 rising 还是 falling。 */
            if (gpio->irq_trigger == GPIO_IRQ_TRIGGER_RISING)
            {
                event = GPIO_IRQ_EVENT_RISING;
            }
            else if (gpio->irq_trigger == GPIO_IRQ_TRIGGER_FALLING)
            {
                event = GPIO_IRQ_EVENT_FALLING;
            }
            else
            {
                event = (gpio_input_bit_get(gpio->gpio_periph, gpio->pin) == RESET) ?
                        GPIO_IRQ_EVENT_FALLING : GPIO_IRQ_EVENT_RISING;
            }

            gpio->irq_cb(slot->dev.name, event, gpio->irq_user_ctx);
        }
    }
}

/* EXTI0~4 为独立中断入口，进入后交给统一分发函数处理。 */
void EXTI0_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_0);
}

void EXTI1_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_1);
}

void EXTI2_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_2);
}

void EXTI3_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_3);
}

void EXTI4_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_4);
}

/* EXTI5~9 共用一个中断入口，需要逐条 line 检查 pending 标志。 */
void EXTI5_9_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_5);
    gd32_gpio_irq_dispatch(EXTI_6);
    gd32_gpio_irq_dispatch(EXTI_7);
    gd32_gpio_irq_dispatch(EXTI_8);
    gd32_gpio_irq_dispatch(EXTI_9);
}

/* EXTI10~15 共用一个中断入口，需要逐条 line 检查 pending 标志。 */
void EXTI10_15_IRQHandler(void)
{
    gd32_gpio_irq_dispatch(EXTI_10);
    gd32_gpio_irq_dispatch(EXTI_11);
    gd32_gpio_irq_dispatch(EXTI_12);
    gd32_gpio_irq_dispatch(EXTI_13);
    gd32_gpio_irq_dispatch(EXTI_14);
    gd32_gpio_irq_dispatch(EXTI_15);
}
