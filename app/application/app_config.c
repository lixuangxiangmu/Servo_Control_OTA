#include "app_config.h"

#include "board_config.h"

#include "app_storage.h"
#include "digital_tube_service/digital_tube_service.h"
#include "encoder_service.h"
#include "feature_config.h"
#include "key_service.h"
#include "led_service.h"
#if CONFIG_USE_OTA
#include "ota_service.h"
#endif
#include "remote_service.h"
#include "return_code.h"
#include "servo_service.h"
#include "storage/eeprom/eeprom.h"
#include "app_mem_map.h"

#define APP_EEPROM_TYPE                 EEPROM_TYPE_AT24C08
#define APP_EEPROM_SLAVE_ADDR           0x50U
#define APP_EEPROM_WRITE_TIMEOUT_MS     20U
#define APP_ENCODER_EVENT_QUEUE_LEN     8U
#define APP_REMOTE_EVENT_QUEUE_LEN      8U

#if CONFIG_USE_OTA
#define APP_OTA_DEFAULT_BLOCK_SIZE      256U
#endif

/************************  LED   ****************************/
static const led_service_item_cfg_t s_led_items[] =
{
    {
        BOARD_SERVO_LED_STATUS_NAME,
        GPIO_LEVEL_LOW,
    },
};

static const led_service_cfg_t s_led_cfg =
{
    s_led_items,
    (uint8_t)(sizeof(s_led_items) / sizeof(s_led_items[0])),
};

/************************  DIGITAL TUBE   ****************************/
/* 数码管产品级绑定配置。
   BSP 负责把这些名字绑定到具体 GPIO 引脚，组件/服务只通过 dev_gpio 访问。 */
static const digital_tube_service_cfg_t s_digital_tube_cfg =
{
    {
        {
            BOARD_DIGITAL_TUBE_SEG_A_NAME,
            BOARD_DIGITAL_TUBE_SEG_B_NAME,
            BOARD_DIGITAL_TUBE_SEG_C_NAME,
            BOARD_DIGITAL_TUBE_SEG_D_NAME,
            BOARD_DIGITAL_TUBE_SEG_E_NAME,
            BOARD_DIGITAL_TUBE_SEG_F_NAME,
            BOARD_DIGITAL_TUBE_SEG_G_NAME,
            BOARD_DIGITAL_TUBE_SEG_DP_NAME,
        },
        {
            BOARD_DIGITAL_TUBE_DIG1_NAME,
            BOARD_DIGITAL_TUBE_DIG2_NAME,
            BOARD_DIGITAL_TUBE_DIG3_NAME,
            BOARD_DIGITAL_TUBE_DIG4_NAME,
        },
        GPIO_LEVEL_LOW,      /* 共阳极，段选低电平点亮。 */
        GPIO_LEVEL_HIGH,     /* 共阳极，位选高电平点亮。 */
    },
    1U,                      /* 每 1ms 刷新一位，四位完整刷新周期约 4ms。 */
    1U,                      /* 初始化后自动启动刷新。 */
};

/************************  KEY   ****************************/
/* 产品按键绑定表。
   服务层只接收 GPIO 设备名和有效电平，不直接依赖 BSP 引脚宏。 */
static const key_service_item_cfg_t s_key_items[] =
{
    {
        BOARD_KEY_BUTTON_NAME,         /* 普通按钮，对应 BSP 中注册的 PA11。 */
        GPIO_LEVEL_LOW,                /* 按键按下为低电平。 */
    },
    {
        BOARD_KEY_ENCODER_NAME,        /* 编码器按键，对应 BSP 中注册的 PA2。 */
        GPIO_LEVEL_LOW,                /* 按键按下为低电平。 */
    },
    {
        BOARD_KEY_ONBOARD_NAME,        /* 板载按键，对应 BSP 中注册的 PB5。 */
        GPIO_LEVEL_LOW,                /* 按键按下为低电平。 */
    },
};

/* 按键扫描配置。
   当前参数兼容老工程：5ms 扫描，50ms 消抖，2s 长按，8s 超长按。 */
static const key_service_cfg_t s_key_cfg =
{
    s_key_items,                                                /* 按键绑定表。 */
    (uint8_t)(sizeof(s_key_items) / sizeof(s_key_items[0])),    /* 按键数量。 */
    5U,                                                         /* 扫描周期，单位 ms。 */
    50U,                                                        /* 消抖确认时间，单位 ms。 */
    2000U,                                                      /* 长按阈值，单位 ms。 */
    8000U,                                                      /* 超长按阈值，单位 ms。 */
};

/************************  ENCODER   ****************************/
/* EC11 编码器产品级绑定配置。
   这里把 BSP 注册的 A/B 相 GPIO 设备名注入 encoder_service；
   服务层只根据 A/B 相 GPIO 事件产生 CW/CCW，不直接修改舵机参数或显示状态。 */
static const encoder_service_item_cfg_t s_encoder_items[] =
{
    {
        BOARD_ENCODER_PHASE_A_NAME,  /* A 相 GPIO 设备名，对应 BSP 中的 PB8。 */
        BOARD_ENCODER_PHASE_B_NAME,  /* B 相 GPIO 设备名，对应 BSP 中的 PB9。 */
        0U,                          /* 方向反转：0 保持默认方向，1 交换 CW/CCW。 */
        2U,                          /* 旋转事件软件消抖时间，单位 ms。 */
    },
};

static const encoder_service_cfg_t s_encoder_cfg =
{
    s_encoder_items,                                                /* 编码器绑定表。 */
    (uint8_t)(sizeof(s_encoder_items) / sizeof(s_encoder_items[0])), /* 编码器数量。 */
    APP_ENCODER_EVENT_QUEUE_LEN,                                    /* 旋转事件队列深度。 */
};

/************************  REMOTE   ****************************/
/* 433MHz 遥控器产品级绑定配置。
   BSP 负责把 GPIO 名和 TIMER 名绑定到 PC5/TIMER4，remote_service 只负责 EV1527 解码和事件上报。 */
static const remote_service_cfg_t s_remote_cfg =
{
    BOARD_REMOTE_RF_GPIO_NAME,          /* 遥控器接收模块数据 GPIO，BSP 中对应 PC5。 */
    BOARD_REMOTE_SAMPLE_TIMER_NAME,     /* 遥控器采样定时器，BSP 中对应 TIMER4。 */
    100U,                               /* 100us 采样周期，兼容老工程 EV1527 解码窗口。 */
    100U,                               /* 第一帧后 100ms 内收到相同第二帧才认为有效。 */
    250U,                               /* 同一键码 250ms 内只上报一次，避免长按时事件刷屏。 */
    APP_REMOTE_EVENT_QUEUE_LEN,         /* 遥控器事件队列深度。 */
    1U,                                 /* 初始化完成后自动启动采样。 */
};

/* 产品舵机绑定配置。
   舵机使用 board.c 中注册的 servo_pwm，角度 0 到 180 度映射到 500us 到 2500us 脉宽。 */
static const servo_service_item_cfg_t s_servo_items[] =
{
    {
        BOARD_SERVO_PWM_NAME,     /* 舵机绑定的 PWM 设备名称。 */
        500U,                     /* 0 度对应的最小脉宽，单位 us。 */
        2500U,                    /* 180 度对应的最大脉宽，单位 us。 */
        1500U,                    /* 初始化脉宽，1500us 对应舵机中位。 */
        0U,                       /* 允许的最小角度。 */
        180U,                     /* 允许的最大角度。 */
        90U,                      /* 初始化角度。 */
    },
};

/* 舵机服务配置，由应用层注入到 servo_service。 */
static const servo_service_cfg_t s_servo_cfg =
{
    s_servo_items,
    (uint8_t)(sizeof(s_servo_items) / sizeof(s_servo_items[0])),
};

static const eeprom_cfg_t s_eeprom_cfg =
{
    APP_STORAGE_EEPROM_NAME,
    BOARD_I2C0_NAME,
    APP_EEPROM_TYPE,
    APP_EEPROM_SLAVE_ADDR,
    APP_EEPROM_WRITE_TIMEOUT_MS,
    0U,
    0U,
    0U,
    0U,
};

int app_components_init(void)
{
    return eeprom_register(&s_eeprom_cfg);
}



/*****************************************************************************
@brief: 使用应用层配置初始化产品服务模块
@para:
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int app_services_init(void)
{
    int ret;

    ret = led_service_init(&s_led_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = digital_tube_service_init(&s_digital_tube_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = key_service_init(&s_key_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = encoder_service_init(&s_encoder_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = remote_service_init(&s_remote_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = servo_service_init(&s_servo_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return RET_OK;
}
