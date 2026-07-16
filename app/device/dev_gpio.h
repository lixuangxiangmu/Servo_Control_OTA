#ifndef DEV_GPIO_H
#define DEV_GPIO_H

#include <stdint.h>

/* GPIO logical level definitions. */
typedef enum
{
    GPIO_LEVEL_LOW = 0,     /* Low level. */
    GPIO_LEVEL_HIGH = 1,    /* High level. */
} gpio_level_t;

/* GPIO working mode definitions. */
typedef enum
{
    GPIO_DEV_MODE_OUTPUT = 0,          /* Push-pull output. */
    GPIO_DEV_MODE_INPUT,               /* Floating input. */
    GPIO_DEV_MODE_INPUT_PULLUP,        /* Input with pull-up. */
    GPIO_DEV_MODE_INPUT_PULLDOWN,      /* Input with pull-down. */
    GPIO_DEV_MODE_OUTPUT_OD,           /* Open-drain output. */
} gpio_mode_t;

/* GPIO 中断触发方式。
   这一层是芯片无关的抽象，具体芯片驱动负责把这些枚举转换为 EXTI/HAL 的触发配置。 */
typedef enum
{
    GPIO_IRQ_TRIGGER_NONE = 0,      /* 不配置中断触发，主要用于初始化/非法参数判断。 */
    GPIO_IRQ_TRIGGER_RISING,        /* 上升沿触发。 */
    GPIO_IRQ_TRIGGER_FALLING,       /* 下降沿触发，EC11 默认使用该模式。 */
    GPIO_IRQ_TRIGGER_BOTH,          /* 双边沿触发，适合需要同时感知高低变化的输入。 */
} gpio_irq_trigger_t;

/* GPIO 中断事件类型。
   驱动层在中断中识别触发边沿后，把事件通过回调上报给服务层。 */
typedef enum
{
    GPIO_IRQ_EVENT_RISING = 0,      /* 上升沿事件。 */
    GPIO_IRQ_EVENT_FALLING,         /* 下降沿事件。 */
} gpio_irq_event_t;

/* GPIO 中断回调函数类型。
   注意：该回调运行在 GPIO/EXTI 中断上下文中，不能调用阻塞 API，不能做耗时业务逻辑。
   name 用于区分触发的 GPIO 逻辑设备，user_ctx 由上层 attach 时传入。 */
typedef void (*gpio_irq_cb_t)(const char *name, gpio_irq_event_t event, void *user_ctx);

/* GPIO 中断配置结构。
   该结构由 device 层传给具体芯片驱动，应用/服务层通常只使用
   dev_gpio_attach_irq()，不直接构造该结构。 */
typedef struct
{
    gpio_irq_trigger_t trigger;     /* 中断触发方式。 */
    gpio_irq_cb_t cb;               /* 中断事件回调函数。 */
    void *user_ctx;                 /* 用户上下文指针，原样传回给 cb。 */
    uint8_t preemption_priority;    /* NVIC 抢占优先级，GD32 驱动会使用该字段配置中断。 */
    uint8_t sub_priority;           /* NVIC 子优先级。 */
} gpio_irq_config_t;

/* GPIO 设备 control 命令。
   上层 dev_gpio_xxx() 接口会把这些命令下发给具体驱动，不建议业务代码直接使用。 */
#define DEV_GPIO_CMD_SET_MODE          1
#define DEV_GPIO_CMD_ATTACH_IRQ        2     /* 绑定并配置 GPIO 中断回调。 */
#define DEV_GPIO_CMD_DETACH_IRQ        3     /* 解绑 GPIO 中断回调并关闭该 EXTI line。 */
#define DEV_GPIO_CMD_ENABLE_IRQ        4     /* 使能或关闭已绑定的 GPIO 中断。 */

/*****************************************************************************
@brief: Set the GPIO working mode
@para:name GPIO device name
@para:mode Target GPIO working mode
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int dev_gpio_set_mode(const char *name, gpio_mode_t mode);

/*****************************************************************************
@brief: Set the GPIO output level
@para:name GPIO device name
@para:level Target output level
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int dev_gpio_write(const char *name, gpio_level_t level);

/*****************************************************************************
@brief: Read the current GPIO input level
@para:name GPIO device name
@para:level Output parameter, used to store the read level
@return: RET_OK indicates read success, other return values indicate failure
*******************************************************************************/
int dev_gpio_read(const char *name, gpio_level_t *level);

/*****************************************************************************
@brief: Toggle the current GPIO output level
@para:name GPIO device name
@return: RET_OK indicates toggle success, other return values indicate failure
*******************************************************************************/
int dev_gpio_toggle(const char *name);

/*****************************************************************************
@brief: 给 GPIO 输入绑定中断回调
@para:name GPIO 逻辑设备名，由 BSP 注册，例如 "encoder_phase_a"
@para:trigger 中断触发方式，常用 GPIO_IRQ_TRIGGER_FALLING
@para:cb 中断回调函数，在 EXTI ISR 上下文中执行
@para:user_ctx 用户私有上下文，会原样传给 cb，常用于区分同一类设备的不同通道
@return: RET_OK 表示成功，其他返回值表示失败
@note: 回调中只能做很轻量的处理，例如发送 FromISR 队列事件，不能阻塞、打印日志或访问 Flash
*******************************************************************************/
int dev_gpio_attach_irq(const char *name, gpio_irq_trigger_t trigger, gpio_irq_cb_t cb, void *user_ctx);

/*****************************************************************************
@brief: 解绑 GPIO 输入中断回调
@para:name GPIO 逻辑设备名
@return: RET_OK 表示成功，其他返回值表示失败
@note: 解绑后该 GPIO 不再向上层上报中断事件
*******************************************************************************/
int dev_gpio_detach_irq(const char *name);

/*****************************************************************************
@brief: 打开或关闭 GPIO 中断事件上报
@para:name GPIO 逻辑设备名
@para:enable 0 表示关闭中断，非 0 表示打开中断
@return: RET_OK 表示成功，其他返回值表示失败
@note: 该接口只控制已经 attach 过的中断，不负责重新配置触发边沿
*******************************************************************************/
int dev_gpio_irq_enable(const char *name, uint8_t enable);

#endif
