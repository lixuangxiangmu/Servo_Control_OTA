# 02_Servo_Control_RT 工程说明

这个工程是基于 GD32F103RET6 + FreeRTOS 的舵机控制工程。当前驱动框架已经从老工程中抽离出比较清晰的分层：应用层不直接碰 GD32 标准库，业务服务不直接绑定端口和引脚，具体 MCU 细节集中在 BSP 和 `drivers/gd32` 中。

README 的目标有两个：

- 新同事看完后，能知道这个工程的初始化顺序、驱动调用链和业务服务怎么组合。
- 把这份说明喂给 AI 后，AI 能按同一套风格仿写新的 GPIO、PWM、ADC、I2C、EEPROM 或其他外设驱动。

---

## 1. 核心思想

本工程按下面的方向调用：

```text
application
  -> services
  -> components
  -> device/dev_xxx
  -> device.c + device_ops_t
  -> drivers/gd32
  -> chip/gd32f10x_standard
```

其中有一个很重要的区别：

```text
运行时调用链: service -> dev_xxx -> device_ops -> drv_xxx -> GD32 标准库
源码依赖方向: driver 依赖 device 和 chip, device 不依赖任何具体 driver
```

也就是说，`device` 层只认识通用 `device_t` 和 `device_ops_t`，并不知道底下是 GD32、STM32 还是别的芯片。具体芯片驱动通过注册函数把自己的操作表挂进 `device` 层，BSP 再负责把板级资源表注册成一个个可按名字查找的逻辑设备。

---

## 2. 当前目录职责

```text
02_Servo_Control_RT/
|-- application/          产品应用层，负责 app_main 和 app_config 注入
|-- bsp/                  板级资源描述，集中管理引脚、外设实例、设备注册
|-- chip/                 GD32F10x 标准库、CMSIS、启动和系统文件
|-- components/           外部器件组件，例如 AT24Cxx EEPROM
|-- config/               工程配置、返回码、FreeRTOSConfig、内存和版本配置
|-- device/               芯片无关设备抽象和 dev_gpio/dev_pwm/dev_adc/dev_i2c 等强类型接口
|-- drivers/gd32/         GD32 内部外设适配层，实现 device_ops_t
|-- kernel/freertos/      FreeRTOS 内核源码
|-- middleware/           芯片无关工具代码
|-- project/iar/          IAR 工程文件
|-- services/             产品业务服务，如舵机、按键、编码器、LED、故障、看门狗
```

分层规则：

- `application` 调用 `services` 和 `components`，不要直接调用 GD32 标准库。
- `services` 调用 `dev_xxx` 或 `components`，不要直接 include `gd32f10x_xxx.h`。
- `components` 封装外部芯片，通过 `dev_i2c/dev_spi/dev_gpio` 等访问硬件。
- `device` 只做注册表、统一分发和芯片无关强类型接口。
- `drivers/gd32` 才允许调用 GD32 标准库，但不要写产品业务逻辑。
- `bsp` 只描述板级资源并注册设备，不写舵机角度、按键动作等业务策略。

---

## 3. 启动流程

当前入口在 `application/main.c`，主流程为：

```c
int main(void)
{
    board_init();
    services_init();
    app_main_init();

    vTaskStartScheduler();

    while (1)
    {
    }
}
```

实际含义：

1. `board_init()`：设置 NVIC 分组，注册并初始化 GPIO、PWM、ADC、I2C 等板级逻辑设备。
2. `services_init()`：初始化框架级服务，目前包括 `fault_service` 和 `watchdog_service`。
3. `app_main_init()`：调用 `app_components_init()` 注册外部器件，再调用 `app_services_init()` 注入并初始化产品业务服务。
4. `vTaskStartScheduler()`：启动 FreeRTOS 调度器。

注意：`while (1) app_main_poll()` 正常情况下不会执行到。当前项目已经是 FreeRTOS 形态，业务轮询应放在任务、软件定时器或服务内部状态机中。

---

## 4. 一条完整调用链

以舵机为例，应用层只关心逻辑舵机 ID：

```c
servo_service_set_angle(SERVO_MAIN, 90);
```

内部链路如下：

```text
servo_service_set_angle(SERVO_MAIN, angle)
  -> 根据 app_config.c 注入的 pwm_name 找到 "servo_pwm"
  -> 将角度映射为 500us 到 2500us 之间的脉宽
  -> dev_pwm_set_pulse_width_us("servo_pwm", pulse_us)
  -> device_find("servo_pwm")
  -> device_control(dev, DEV_PWM_CMD_SET_PULSE_WIDTH_US, &pulse_us)
  -> gd32_pwm_control()
  -> timer_channel_output_pulse_value_config()
```

这个链路说明了本工程的驱动设计风格：

- 应用层只用业务语义，例如 `SERVO_MAIN` 和角度。
- 服务层负责业务换算，例如角度到 PWM 脉宽。
- `dev_pwm` 负责把强类型 API 翻译成 `device_control` 命令。
- `device` 负责按名字找设备并分发到 `device_ops_t`。
- `drv_pwm` 负责操作 GD32 定时器寄存器和标准库 API。

---

## 5. Device 抽象层怎么工作

`device/device.h` 定义了统一设备对象：

```c
typedef struct
{
    int (*init)(device_t *dev);
    int (*open)(device_t *dev);
    int (*close)(device_t *dev);
    int (*read)(device_t *dev, uint8_t *buf, uint32_t len);
    int (*write)(device_t *dev, const uint8_t *buf, uint32_t len);
    int (*control)(device_t *dev, int cmd, void *arg);
} device_ops_t;

struct device
{
    const char *name;
    device_class_t type;
    const device_ops_t *ops;
    void *user_data;
    uint16_t flags;
    uint8_t initialized;
    uint8_t opened;
    uint8_t ref_count;
};
```

驱动注册时要做三件事：

1. 从静态池里拿一个 slot，slot 内部同时包含 `device_t dev` 和私有硬件上下文。
2. 填好 `dev.name/dev.type/dev.ops/dev.user_data`。
3. 调用 `device_register(&slot->dev)`。

典型模式：

```c
typedef struct
{
    device_t dev;
    xxx_hw_context_t hw;
} xxx_slot_t;

static xxx_slot_t s_xxx_slots[XXX_DEVICE_MAX];
static uint8_t s_xxx_count;

int gd32_xxx_register(const gd32_xxx_cfg_t *cfg)
{
    xxx_slot_t *slot = &s_xxx_slots[s_xxx_count];

    slot->hw = ...;                 /* 保存外设基地址、引脚、通道、默认配置 */
    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_XXX;
    slot->dev.ops = &s_gd32_xxx_ops;
    slot->dev.user_data = &slot->hw;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_xxx_count++;
    }

    return ret;
}
```

`user_data` 是这套框架的关键：上层拿到的是通用 `device_t *`，底层驱动再通过 `dev->user_data` 还原成自己的硬件上下文。

---

## 6. BSP 资源注册方式

`bsp/board.c` 是板级资源总表。当前已经注册：

- GPIO：状态 LED、普通按键、编码器按键、板载按键、EC11 A/B 相。
- PWM：`servo_pwm`，PC6 + TIMER7_CH0，50Hz。
- ADC：`adc_power_raw` 和 `adc_locked_rotor_raw`，ADC0 + DMA0_CH0 连续扫描。
- I2C：`i2c0`，PB6/PB7，400kHz。

BSP 的典型写法是资源表 + 批量注册：

```c
static const gd32_pwm_cfg_t s_board_pwms[] =
{
    {
        BOARD_SERVO_PWM_NAME,
        RCU_TIMER7,
        RCU_GPIOC,
        TIMER7,
        TIMER_CH_0,
        GPIOC,
        GPIO_PIN_6,
        GPIO_MODE_AF_PP,
        GPIO_OSPEED_10MHZ,
        0U,
        { 50U, 75U, PWM_POLARITY_NORMAL },
    },
};

static void board_pwms_register(void)
{
    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_pwms); i++)
    {
        if (gd32_pwm_register(&s_board_pwms[i]) == RET_OK)
        {
            dev = device_find(s_board_pwms[i].name);
            (void)device_init(dev);
        }
    }
}
```

原则：

- 设备名统一放在 `bsp/board_config.h`，例如 `BOARD_SERVO_PWM_NAME`。
- 服务层通过配置注入设备名，不直接依赖端口、引脚和通道。
- 新增外设时先补 BSP 资源表，再补 `board_xxx_register()`。

---

## 7. Application 配置注入

`application/app_config.c` 是产品级绑定层，负责把“板级设备名”注入“业务服务”。

例如舵机服务：

```c
static const servo_service_item_cfg_t s_servo_items[] =
{
    {
        BOARD_SERVO_PWM_NAME,
        500U,
        2500U,
        1500U,
        0U,
        180U,
        90U,
    },
};

static const servo_service_cfg_t s_servo_cfg =
{
    s_servo_items,
    (uint8_t)(sizeof(s_servo_items) / sizeof(s_servo_items[0])),
};
```

这样做的好处是：

- BSP 负责“这个名字对应哪个硬件”。
- `app_config.c` 负责“这个硬件用于哪个业务”。
- 服务层只接收配置结构，不直接 include `board_config.h`。

新增业务服务时，推荐也按这个模式：

```c
typedef struct
{
    const char *device_name;
    uint32_t param_a;
    uint32_t param_b;
} xxx_service_item_cfg_t;

typedef struct
{
    const xxx_service_item_cfg_t *items;
    uint8_t item_count;
} xxx_service_cfg_t;

int xxx_service_init(const xxx_service_cfg_t *cfg);
```

---

## 8. 已完成驱动和服务要点

### 8.1 GPIO

接口在 `device/dev_gpio.h`。

常用 API：

```c
dev_gpio_write(name, GPIO_LEVEL_HIGH);
dev_gpio_read(name, &level);
dev_gpio_toggle(name);
dev_gpio_attach_irq(name, GPIO_IRQ_TRIGGER_FALLING, cb, user_ctx);
dev_gpio_irq_enable(name, 1U);
```

特点：

- GD32 驱动支持普通输入输出和 EXTI 中断。
- EXTI0-4 独立分发，EXTI5-9 和 EXTI10-15 会逐 line 检查 pending。
- 中断回调运行在 ISR 上下文，只能做轻量处理，例如投递 `FromISR` 队列事件。

### 8.2 PWM

接口在 `device/dev_pwm.h`。

常用 API：

```c
pwm_config_t cfg = { 50U, 0U, PWM_POLARITY_NORMAL };
dev_pwm_config("servo_pwm", &cfg);
dev_pwm_set_pulse_width_us("servo_pwm", 1500U);
dev_pwm_start("servo_pwm");
```

特点：

- GD32 PWM 驱动内部把计数频率固定成 1MHz，因此计数单位就是 1us。
- 舵机控制优先使用 `dev_pwm_set_pulse_width_us()`，比换算千分比占空比更直观。
- `servo_service` 将角度映射到脉宽，默认 0 到 180 度映射 500us 到 2500us。

### 8.3 ADC

接口在 `device/dev_adc.h`。

常用 API：

```c
uint16_t raw;
dev_adc_read_raw(BOARD_ADC_POWER_RAW_NAME, &raw);
```

特点：

- 当前 ADC 使用“物理扫描组 + 多个逻辑 ADC 设备”的设计。
- ADC0 通过 DMA0_CH0 循环扫描，结果保存在 `samples[]`。
- 每个逻辑通道按名字注册成一个 device，例如 `adc_power_raw`。
- 读取时不重新触发转换，只读取 DMA 缓存中的最新 raw 值，接口轻量不阻塞。

### 8.4 I2C

接口在 `device/dev_i2c.h`。

常用 API：

```c
dev_i2c_mem_write("i2c0", 0x50, reg, 1U, data, len, 100U);
dev_i2c_mem_read("i2c0", 0x50, reg, 1U, data, len, 100U);
dev_i2c_scan("i2c0", addr_buf, max_num, &found_num);
dev_i2c_bus_recover("i2c0");
```

特点：

- 当前 GD32 I2C 驱动支持 7 位主机模式，最高 400kHz。
- 支持寄存器地址宽度 0/1/2 字节。
- 读寄存器使用“写地址前缀 + repeated start + read”的模式。
- 提供 scan、probe、bus recover，便于调试外部器件。

### 8.5 EEPROM 组件

接口在 `components/storage/eeprom/eeprom.h`。

常用 API：

```c
eeprom_register(&cfg);
eeprom_read("eeprom0", addr, buf, len);
eeprom_write("eeprom0", addr, buf, len);
eeprom_update("eeprom0", addr, buf, len);
```

特点：

- EEPROM 是外部器件，所以放在 `components`，不是 `drivers/gd32`。
- 底层通过 `dev_i2c` 访问总线，不直接调用 GD32 I2C 标准库。
- 支持 AT24C08、AT24C32 和自定义容量参数。
- 写入时自动处理页边界、块地址边界和写周期 ACK 轮询。
- `eeprom_update()` 会先读后比对，数据不变时不写，减少 EEPROM 擦写损耗。

---

## 9. 当前业务服务

### 9.1 LED 服务

`led_service` 通过配置表绑定 GPIO 名和有效电平。

```c
led_service_on(LED_STATUS);
led_service_off(LED_STATUS);
led_service_toggle(LED_STATUS);
led_service_blink_start(LED_STATUS, 500U, 0U);
```

服务层只认 `LED_STATUS`，不关心 LED 接在哪个引脚，也不关心是高电平亮还是低电平亮。

### 9.2 Key 服务

`key_service` 使用 FreeRTOS 软件定时器周期扫描按键，并把短按、长按、超长按事件放进队列。

当前默认配置：

- 扫描周期：5ms。
- 消抖：50ms。
- 长按：2000ms。
- 超长按：8000ms。
- 队列深度：8。

应用任务中读取：

```c
key_event_t event;

if (key_service_get_event(&event, portMAX_DELAY) == RET_OK)
{
    switch (event.key_id)
    {
    case KEY_ONBOARD:
        if (event.press_type == KEY_PRESS_SHORT)
        {
            led_service_toggle(LED_STATUS);
        }
        break;

    default:
        break;
    }
}
```

### 9.3 Encoder 服务

`encoder_service` 给 EC11 A/B 相 GPIO 绑定下降沿中断。ISR 中只判向并投递队列，真正业务由应用任务消费。

判向规则：

- A 相下降沿时，如果 B 相为低，默认上报 CCW。
- B 相下降沿时，如果 A 相为低，默认上报 CW。
- 如果机械安装方向相反，通过配置或 `encoder_service_set_reverse()` 反转。

读取事件：

```c
encoder_event_t event;

if (encoder_service_get_event(&event, portMAX_DELAY) == RET_OK)
{
    if (event.rotate == ENCODER_ROTATE_CW)
    {
        /* increase value */
    }
    else if (event.rotate == ENCODER_ROTATE_CCW)
    {
        /* decrease value */
    }
}
```

ISR 技巧：

- GPIO ISR 回调里可以调用 `dev_gpio_read()` 读取另一相电平。
- 队列投递必须使用 `xQueueSendFromISR()`。
- ISR 里不要打印日志、不要阻塞、不要写 Flash/EEPROM。

### 9.4 Servo 服务

`servo_service` 把逻辑角度转换为 PWM 脉宽。

当前默认配置：

- PWM 频率：50Hz。
- 最小脉宽：500us。
- 最大脉宽：2500us。
- 初始脉宽：1500us。
- 角度范围：0 到 180 度。
- 初始角度：90 度。

调用：

```c
servo_service_set_angle(SERVO_MAIN, 90U);
servo_service_get_angle(SERVO_MAIN, &angle);
```

### 9.5 Fault 和 Watchdog 服务

`fault_service` 是统一故障收口，所有模块应使用统一故障码和等级上报，不要各自维护私有故障状态。

`watchdog_service` 是喂狗策略层，硬件喂狗回调可注入。正确模式是关键模块定期 mark alive，只有 required mask 都满足时才喂狗。

```c
watchdog_service_mark_alive(WDG_ALIVE_MAIN);
watchdog_service_poll();
```

不要在中断、delay、日志输出或某个业务角落偷偷喂狗。

---

## 10. 返回值约定

统一返回码在 `config/return_code.h`：

```c
RET_OK = 0
RET_FAIL = -1
RET_INVALID_PARAM = -2
RET_NOT_FOUND = -3
RET_NO_RESOURCE = -4
RET_NOT_SUPPORTED = -6
RET_TIMEOUT = -8
RET_NOT_INITED = -12
RET_IO_ERROR = -13
```

约定：

- `0` 表示纯成功。
- 负数表示错误。
- 读写类接口可以返回正数表示实际读写长度。
- 用 `RET_IS_ERR(ret)` 判断错误，用 `RET_IS_OK(ret)` 判断等于 `RET_OK`。

注意：如果接口可能返回正数成功值，不要只用 `RET_IS_OK(ret)` 判断成功。

---

## 11. 仿写一个新驱动的步骤

假设要新增 `dev_timer` 或 `dev_uart`，按下面步骤走：

1. 在 `device/` 增加芯片无关接口。

```c
/* dev_xxx.h */
typedef struct
{
    uint32_t param;
} xxx_config_t;

#define DEV_XXX_CMD_CONFIG  1
#define DEV_XXX_CMD_START   2
#define DEV_XXX_CMD_STOP    3

int dev_xxx_config(const char *name, const xxx_config_t *cfg);
int dev_xxx_start(const char *name);
int dev_xxx_stop(const char *name);
```

2. 在 `device/dev_xxx.c` 中封装 `device_find()` 和 `device_control()`。

```c
int dev_xxx_start(const char *name)
{
    device_t *dev = device_find(name);

    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    return device_control(dev, DEV_XXX_CMD_START, 0);
}
```

3. 在 `drivers/gd32/` 增加 GD32 配置结构和注册函数。

```c
typedef struct
{
    const char *name;
    uint32_t xxx_periph;
    rcu_periph_enum xxx_rcu;
    xxx_config_t default_config;
} gd32_xxx_cfg_t;

int gd32_xxx_register(const gd32_xxx_cfg_t *cfg);
```

4. 在 `drivers/gd32/drv_xxx.c` 中实现静态 slot 池和 `device_ops_t`。

```c
static const device_ops_t s_gd32_xxx_ops =
{
    gd32_xxx_init,
    gd32_xxx_open,
    gd32_xxx_close,
    gd32_xxx_read,
    gd32_xxx_write,
    gd32_xxx_control,
};
```

5. 在 `bsp/board.c` 增加资源表和注册函数。

```c
static const gd32_xxx_cfg_t s_board_xxxs[] =
{
    {
        BOARD_XXX_NAME,
        XXX0,
        RCU_XXX0,
        { ... },
    },
};
```

6. 在 `bsp/board_config.h` 增加逻辑设备名。

```c
#define BOARD_XXX_NAME "xxx0"
```

7. 如果是业务模块使用，在 `application/app_config.c` 把设备名注入对应 service。

8. 编译后按顺序验证：注册成功、`device_find()` 能找到、`device_init()` 成功、`dev_xxx` API 成功、service 业务正常。

---

## 12. 写驱动时的关键技巧

### 12.1 不用动态内存

驱动层统一使用静态池：

```c
static gd32_xxx_slot_t s_xxx_slots[GD32_XXX_DEVICE_MAX];
static uint8_t s_xxx_count;
```

这样适合 MCU 工程，启动后资源数量确定，也便于定位 `RET_NO_RESOURCE`。

### 12.2 注册前先检查参数和重名

最少要检查：

- `cfg != 0`。
- `cfg->name != 0`。
- 关键硬件参数合法。
- `device_find(cfg->name) == 0`。
- 静态 slot 池未满。

对于 ADC 这种一次注册多个逻辑设备的驱动，要先把所有名字查重，再开始注册，避免注册一半失败后状态不完整。

### 12.3 init 只初始化硬件，不写业务策略

`gd32_xxx_init()` 应只做：

- 开时钟。
- 配 GPIO 复用。
- 配外设寄存器。
- 配 DMA/中断。
- 设置默认硬件状态。

不要在 driver 里做“按键控制舵机”“错误后闪灯”这类业务动作。

### 12.4 control 用于非 read/write 的操作

GPIO 的设置模式、中断绑定；PWM 的频率、占空比、脉宽；I2C 的 transfer、scan、recover，都走 `device_control()`。

建议：

- 常用操作在 `dev_xxx.h` 做强类型 API。
- 特殊操作才在内部使用 `DEV_XXX_CMD_xxx`。
- 业务层不要直接调用裸 `device_control()`，优先调用 `dev_xxx_xxx()`。

### 12.5 ISR 只投递事件

当前工程已经有两个典型例子：

- GPIO EXTI 驱动只清中断并派发 GPIO 回调。
- 编码器服务在 ISR 回调里判向，并用 `xQueueSendFromISR()` 投递事件。

ISR 中不要做：

- 阻塞等待。
- EEPROM/Flash 写入。
- 大量计算。
- printf/log 输出。
- 普通 FreeRTOS API。

### 12.6 服务层用逻辑 ID，对外不要暴露设备名

例如：

```c
servo_service_set_angle(SERVO_MAIN, angle);
led_service_toggle(LED_STATUS);
key_service_get_event(&event, timeout);
```

设备名只在 `app_config.c` 的配置表中出现。这样业务代码不会到处散落 `"servo_pwm"`、`"key_button"` 这类字符串。

---

## 13. 迁移或扩展建议

### 换 MCU

主要改：

- `chip/`
- `drivers/<new_chip>/`
- `bsp/`
- `config/project_config.h`
- FreeRTOS portable 和启动文件

尽量不改：

- `application/`
- `services/`
- `components/`
- `device/`

### 换开发板

主要改：

- `bsp/board.c`
- `bsp/board_config.h`

如果设备名保持不变，上层服务基本不用动。

### 新增外部器件

放到 `components/`，例如传感器、EEPROM、显示屏、外部 IO 扩展。

组件内部通过 `dev_i2c/dev_spi/dev_gpio` 访问硬件，不直接调用 GD32 标准库。

### 新增产品业务

放到 `services/` 或 `application/`：

- 可复用、独立的业务能力放 `services`。
- 只属于当前产品流程的状态机、任务编排放 `application`。

---

## 14. 常见错误

- 在 `services` 里 include `gd32f10x_xxx.h`：应该改为调用 `dev_xxx`。
- 在 `drivers/gd32` 里写业务逻辑：驱动只做硬件适配。
- 在多个文件里硬编码 `"servo_pwm"`：设备名集中在 `board_config.h` 和 `app_config.c`。
- 注册多个设备时共用同一个 static `device_t`：应使用 slot 数组，每个逻辑设备一个 `device_t`。
- ISR 中调用阻塞 API：必须使用 FromISR 版本或只置标志。
- 读写接口返回正数时被当成错误：正数通常表示实际长度。
- BSP 中只注册不初始化，导致第一次读写返回 `RET_NOT_INITED`：当前板级注册后会立即 `device_init()`。
- EEPROM 页写跨页：组件已经处理，新的存储组件也应处理页/块边界。

---

## 15. AI 仿写提示词

如果要让 AI 参考本工程写新驱动，可以直接给它下面这段要求：

```text
请按 02_Servo_Control_RT 当前驱动风格实现一个新的外设驱动：
1. device 层提供芯片无关 dev_xxx.h/c 强类型接口。
2. drivers/gd32 层实现 gd32_xxx_cfg_t、静态 slot 池、device_ops_t 和 gd32_xxx_register()。
3. BSP 在 board.c 中用 const 资源表描述硬件，并在 board_device_init() 中注册和 device_init()。
4. service/application 不直接 include GD32 标准库，只通过 dev_xxx 或组件接口访问硬件。
5. 统一返回 config/return_code.h 中的 RET_xxx，读写类成功可返回实际长度。
6. 不使用动态内存；多实例用静态数组；注册前检查参数、重名和资源池容量。
7. ISR 中只做清标志和事件投递，FreeRTOS 必须使用 FromISR API。
8. 如果是外部器件，放 components，不放 drivers/gd32，底层通过 dev_i2c/dev_spi/dev_gpio 访问。
```

---

## 16. 一句话总结

这个工程的核心不是某一个外设驱动，而是一套固定套路：

```text
BSP 描述硬件资源 -> driver 注册 device_ops -> device 统一分发
-> dev_xxx 提供强类型接口 -> app_config 注入设备名
-> service 写业务语义 -> application 编排产品行为
```

照着这个链路写，新驱动会自然保持同一套调用逻辑，后续换芯片、换板子、加外部器件也会轻松很多。
