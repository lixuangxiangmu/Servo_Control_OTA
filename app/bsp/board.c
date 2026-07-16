#include "board.h"

#include "board_config.h"
#include "project_config.h"
#include "dev_gpio.h"
#include "device.h"
#include "gd32/drv_adc.h"
#include "gd32/drv_gpio.h"
#include "gd32/drv_i2c.h"
#include "gd32/drv_pwm.h"
#include "gd32/drv_timer.h"
#include "gd32/drv_uart.h"
#include "return_code.h"

#include <stdint.h>

#define BOARD_ARRAY_SIZE(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))


#if defined(CONFIG_CHIP_GD32)
/* 板级 GPIO 资源表。
   按键沿用老工程硬件连接：普通按钮 PA11，编码器按键 PA2，板载按键 PB5。
   三个按键均配置为浮空输入，按键服务通过应用配置指定低电平有效。 */
static const gd32_gpio_cfg_t s_board_gpios[] =
{
    {
        BOARD_SERVO_LED_STATUS_NAME,
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_10,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_SERVO_POWER_HOLD_NAME, /* 舵机保持供电控制，沿用老工程 power2：PB12 高电平打开。 */
        RCU_GPIOB,
        GPIOB,
        GPIO_PIN_12,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_LOW
    },
    {
        BOARD_KEY_BUTTON_NAME,     /* 普通按钮 GPIO 设备名称。 */
        RCU_GPIOA,                 /* 普通按钮位于 GPIOA，需打开 GPIOA 时钟。 */
        GPIOA,                     /* 普通按钮端口。 */
        GPIO_PIN_11,               /* 普通按钮引脚：PA11。 */
        GPIO_MODE_IN_FLOATING,     /* 按键输入模式，保持与老工程一致。 */
        GPIO_OSPEED_2MHZ,          /* 输入引脚速度参数。 */
        GPIO_LEVEL_HIGH            /* 输入设备不使用默认输出电平，保留字段填高电平。 */
    },
    {
        BOARD_KEY_ENCODER_NAME,    /* 编码器按键 GPIO 设备名称。 */
        RCU_GPIOA,                 /* 编码器按键位于 GPIOA，需打开 GPIOA 时钟。 */
        GPIOA,                     /* 编码器按键端口。 */
        GPIO_PIN_2,                /* 编码器按键引脚：PA2。 */
        GPIO_MODE_IN_FLOATING,     /* 按键输入模式，保持与老工程一致。 */
        GPIO_OSPEED_2MHZ,          /* 输入引脚速度参数。 */
        GPIO_LEVEL_HIGH            /* 输入设备不使用默认输出电平，保留字段填高电平。 */
    },
    {
        BOARD_KEY_ONBOARD_NAME,    /* 板载按键 GPIO 设备名称。 */
        RCU_GPIOB,                 /* 板载按键位于 GPIOB，需打开 GPIOB 时钟。 */
        GPIOB,                     /* 板载按键端口。 */
        GPIO_PIN_5,                /* 板载按键引脚：PB5。 */
        GPIO_MODE_IN_FLOATING,     /* 按键输入模式，保持与老工程一致。 */
        GPIO_OSPEED_2MHZ,          /* 输入引脚速度参数。 */
        GPIO_LEVEL_HIGH            /* 输入设备不使用默认输出电平，保留字段填高电平。 */
    },
    {
        BOARD_ENCODER_PHASE_A_NAME, /* EC11 A 相 GPIO 设备名称。 */
        RCU_GPIOB,                  /* A 相位于 GPIOB，需要打开 GPIOB 时钟。 */
        GPIOB,                      /* A 相端口。 */
        GPIO_PIN_8,                 /* A 相引脚：PB8，沿用老工程编码器 A 相接线。 */
        GPIO_MODE_IN_FLOATING,      /* EC11 外部已有电路约束，保持与老工程一致的浮空输入。 */
        GPIO_OSPEED_2MHZ,           /* 输入引脚速度参数。 */
        GPIO_LEVEL_HIGH             /* 输入设备不使用默认输出电平，保留字段填高电平。 */
    },
    {
        BOARD_ENCODER_PHASE_B_NAME, /* EC11 B 相 GPIO 设备名称。 */
        RCU_GPIOB,                  /* B 相同样位于 GPIOB。 */
        GPIOB,                      /* B 相端口。 */
        GPIO_PIN_9,                 /* B 相引脚：PB9，沿用老工程编码器 B 相接线。 */
        GPIO_MODE_IN_FLOATING,      /* 与 A 相一致，配置为浮空输入。 */
        GPIO_OSPEED_2MHZ,           /* 输入引脚速度参数。 */
        GPIO_LEVEL_HIGH             /* 输入设备不使用默认输出电平，保留字段填高电平。 */
    },
    {
        BOARD_REMOTE_RF_GPIO_NAME, /* 433MHz 遥控器接收模块数据输入，沿用老工程 PC5。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_5,
        GPIO_MODE_IPU,
        GPIO_OSPEED_10MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_A_NAME, /* 数码管 A 段，老工程连接 PA0。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_0,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_B_NAME, /* 数码管 B 段，老工程连接 PC0。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_0,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_C_NAME, /* 数码管 C 段，老工程连接 PA5。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_5,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_D_NAME, /* 数码管 D 段，老工程连接 PA7。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_7,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_E_NAME, /* 数码管 E 段，老工程连接 PC4。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_4,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_F_NAME, /* 数码管 F 段，老工程连接 PC3。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_3,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_G_NAME, /* 数码管 G 段，老工程连接 PA4。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_4,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_SEG_DP_NAME, /* 数码管 DP 段，老工程连接 PA6。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_6,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_HIGH
    },
    {
        BOARD_DIGITAL_TUBE_DIG1_NAME, /* 数码管第 1 位位选，老工程连接 PA1。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_1,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_LOW
    },
    {
        BOARD_DIGITAL_TUBE_DIG2_NAME, /* 数码管第 2 位位选，老工程连接 PC2。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_2,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_LOW
    },
    {
        BOARD_DIGITAL_TUBE_DIG3_NAME, /* 数码管第 3 位位选，老工程连接 PC1。 */
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_1,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_LOW
    },
    {
        BOARD_DIGITAL_TUBE_DIG4_NAME, /* 数码管第 4 位位选，老工程连接 PA3。 */
        RCU_GPIOA,
        GPIOA,
        GPIO_PIN_3,
        GPIO_MODE_OUT_PP,
        GPIO_OSPEED_2MHZ,
        GPIO_LEVEL_LOW
    },
};

/* 板级 PWM 资源配置表。
   当前舵机 PWM 复用老工程硬件资源：GPIOC.6 + TIMER7_CH0。 */
static const gd32_pwm_cfg_t s_board_pwms[] =
{
    {
        BOARD_SERVO_PWM_NAME,     /* 注册到设备层的 PWM 设备名称。 */
        RCU_TIMER7,               /* 舵机 PWM 使用 TIMER7 外设时钟。 */
        RCU_GPIOC,                /* PWM 输出引脚所在 GPIOC 时钟。 */
        TIMER7,                   /* 舵机 PWM 使用 TIMER7。 */
        TIMER_CH_0,               /* 舵机 PWM 使用 TIMER7 通道 0。 */
        GPIOC,                    /* PWM 输出引脚所在 GPIO 端口。 */
        GPIO_PIN_6,               /* PWM 输出引脚为 PC6。 */
        GPIO_MODE_AF_PP,          /* PWM 输出需要配置为复用推挽模式。 */
        GPIO_OSPEED_10MHZ,        /* PWM 输出引脚速度。 */
        0U,                       /* 使用系统时钟作为定时器源时钟。 */
        {
            50U,                  /* 舵机 PWM 标准频率 50Hz，对应 20ms 周期。 */
            75U,                  /* 默认占空比 7.5%，对应 1500us 中位脉宽。 */
            PWM_POLARITY_NORMAL,  /* 正常极性，高电平为有效脉冲。 */
        },
    },
};

/* 板级 ADC0 扫描通道配置。
   当前复用老工程硬件连接：
   - PB0 -> ADC_CHANNEL_8，用于电源电压相关 raw 采样；
   - PB1 -> ADC_CHANNEL_9，用于堵转检测相关 raw 采样。
   每一项最终都会注册成一个可通过名称查找的 ADC 逻辑设备。 */
static const gd32_adc_channel_cfg_t s_board_adc0_channels[] =
{
    {
        BOARD_ADC_POWER_RAW_NAME,
        RCU_GPIOB,
        GPIOB,
        GPIO_PIN_0,
        ADC_CHANNEL_8,
        ADC_SAMPLETIME_239POINT5
    },
    {
        BOARD_ADC_LOCKED_ROTOR_RAW_NAME,
        RCU_GPIOB,
        GPIOB,
        GPIO_PIN_1,
        ADC_CHANNEL_9,
        ADC_SAMPLETIME_239POINT5
    },
};

/* 板级 ADC 扫描组配置。
   ADC0 使用 DMA0_CH0 环形搬运扫描结果，驱动内部会把每路 raw 值缓存到 samples[]。 */
static const gd32_adc_cfg_t s_board_adcs[] =
{
    {
        ADC0,
        RCU_ADC0,
        RCU_CKADC_CKAPB2_DIV6,
        DMA0,
        RCU_DMA0,
        DMA_CH0,
        s_board_adc0_channels,
        (uint8_t)BOARD_ARRAY_SIZE(s_board_adc0_channels)
    },
};

static const gd32_i2c_cfg_t s_board_i2cs[] =
{
    {
        BOARD_I2C0_NAME,
        I2C0,
        RCU_I2C0,
        RCU_GPIOB,
        GPIOB,
        GPIO_PIN_6,
        RCU_GPIOB,
        GPIOB,
        GPIO_PIN_7,
        {
            400000U,
            I2C_ADDR_7BIT,
            0x0AU
        }
    },
};

static const gd32_uart_cfg_t s_board_uarts[] =
{
    {
        BOARD_UART3_NAME,
        UART3,
        RCU_UART3,
        RCU_GPIOC,
        GPIOC,
        GPIO_PIN_10,
        GPIO_PIN_11,
        UART3_IRQn,
        DMA1,
        RCU_DMA1,
        DMA_CH2,
        DMA_CH4,
        DMA1_Channel3_Channel4_IRQn,
        6U,
        0U,
        {
            115200U,
            UART_DATA_BITS_8,
            UART_STOP_BITS_1,
            UART_PARITY_NONE
        }
    },
};

/* 板级周期定时器资源配置表。
   433MHz 遥控器使用 TIMER4 提供 100us 采样节拍，协议解析放在 remote_service 中。 */
static const gd32_timer_cfg_t s_board_timers[] =
{
    {
        BOARD_REMOTE_SAMPLE_TIMER_NAME,
        RCU_TIMER4,
        TIMER4,
        TIMER4_IRQn,
        0U,
        6U,
        0U,
        {
            100U,       //100us定时器中断
        },
    },
};

/*****************************************************************************
@brief: 注册并初始化板级 GPIO 设备
@para:
@return:
*******************************************************************************/
static void board_gpios_register(void)
{
    uint8_t i;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_gpios); i++)
    {
        if (gd32_gpio_register(&s_board_gpios[i]) == RET_OK)
        {
            dev = device_find(s_board_gpios[i].name);
            (void)device_init(dev);
        }
    }
}

/*****************************************************************************
@brief: 注册并初始化板级 PWM 设备
@para:
@return:
*******************************************************************************/
static void board_pwms_register(void)
{
    uint8_t i;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_pwms); i++)
    {
        if (gd32_pwm_register(&s_board_pwms[i]) == RET_OK)
        {
            dev = device_find(s_board_pwms[i].name);
            (void)device_init(dev);
        }
    }
}

/*****************************************************************************
@brief: 注册并初始化板级 ADC 设备
@para:
@return:
*******************************************************************************/
static void board_adcs_register(void)
{
    uint8_t i;
    uint8_t j;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_adcs); i++)
    {
        if (gd32_adc_register(&s_board_adcs[i]) == RET_OK)
        {
            /* 注册成功后立即初始化组内每个逻辑 ADC 设备。
               多个逻辑通道共享同一个扫描组，底层会避免重复初始化 ADC0/DMA0。 */
            for (j = 0U; j < s_board_adcs[i].channel_count; j++)
            {
                dev = device_find(s_board_adcs[i].channels[j].name);
                (void)device_init(dev);
            }
        }
    }
}

/*****************************************************************************
@brief: 注册并初始化板级 IIC 设备
@para:
@return:
*******************************************************************************/
static void board_i2cs_register(void)
{
    uint8_t i;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_i2cs); i++)
    {
        if (gd32_i2c_register(&s_board_i2cs[i]) == RET_OK)
        {
            dev = device_find(s_board_i2cs[i].name);
            (void)device_init(dev);
        }
    }
}

/*****************************************************************************
@brief: 注册并初始化板级 UART 设备
@para:
@return:
*******************************************************************************/
static void board_uarts_register(void)
{
    uint8_t i;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_uarts); i++)
    {
        if (gd32_uart_register(&s_board_uarts[i]) == RET_OK)
        {
            dev = device_find(s_board_uarts[i].name);
            (void)device_init(dev);
        }
    }
}

/*****************************************************************************
@brief: 注册并初始化板级周期定时器设备
@para:
@return:
*******************************************************************************/
static void board_timers_register(void)
{
    uint8_t i;
    device_t *dev;

    for (i = 0U; i < BOARD_ARRAY_SIZE(s_board_timers); i++)
    {
        if (gd32_timer_register(&s_board_timers[i]) == RET_OK)
        {
            dev = device_find(s_board_timers[i].name);
            (void)device_init(dev);
        }
    }
}

#endif

/*****************************************************************************
@brief: 注册板级设备资源
@para:
@return:
*******************************************************************************/
static void board_device_init(void)
{
#if defined(CONFIG_CHIP_GD32)
    board_gpios_register();
    board_pwms_register();
    board_adcs_register();
    board_i2cs_register();
    board_uarts_register();
    board_timers_register();
#endif
}

/*****************************************************************************
@brief: 初始化板级硬件资源
@para:
@return:
*******************************************************************************/
void board_init(void)
{
#if defined(CONFIG_CHIP_GD32)
    nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);
#endif
    board_device_init();
}
