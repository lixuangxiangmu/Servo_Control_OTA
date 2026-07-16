#ifndef KEY_SERVICE_H
#define KEY_SERVICE_H

#include "dev_gpio.h"

#include <stdint.h>

/* 产品按键逻辑 ID 定义，上层只使用逻辑 ID，不关心具体 GPIO 引脚。 */
typedef enum
{
    KEY_BUTTON = 0,       /* 普通按钮。 */
    KEY_ENCODER,          /* 编码器自带按键。 */
    KEY_ONBOARD,          /* 板载按键。 */
    KEY_ID_MAX,           /* 按键数量边界值，不作为实际按键使用。 */
} key_id_t;

/* 按键服务上报的按压类型定义。 */
typedef enum
{
    KEY_PRESS_NONE = 0,       /* 无按键事件。 */
    KEY_PRESS_SHORT,          /* 短按，按下超过消抖时间后释放触发。 */
    KEY_PRESS_LONG,           /* 长按，按下超过长按阈值后释放触发。 */
    KEY_PRESS_VERY_LONG,      /* 超长按，按下超过超长按阈值后释放触发。 */
} key_press_type_t;

/* 单个按键的硬件绑定配置，由 application/app_config.c 注入。 */
typedef struct
{
    const char *gpio_name;          /* 绑定到 device 层的 GPIO 设备名称。 */
    gpio_level_t active_level;      /* 按键按下时的有效电平，当前电路为低电平有效。 */
} key_service_item_cfg_t;

/* 按键服务总配置，由应用配置层统一提供。 */
typedef struct
{
    const key_service_item_cfg_t *items;    /* 按键配置表。 */
    uint8_t item_count;                     /* 按键配置数量。 */
    uint16_t scan_period_ms;                /* 软件定时器扫描周期，单位 ms。 */
    uint32_t debounce_ms;                   /* 消抖确认时间，单位 ms。 */
    uint32_t long_press_ms;                 /* 长按判定阈值，单位 ms。 */
    uint32_t very_long_press_ms;            /* 超长按判定阈值，单位 ms。 */
} key_service_cfg_t;

/* 按键服务对外上报的事件结构体。 */
typedef struct
{
    key_id_t key_id;                    /* 触发事件的按键逻辑 ID。 */
    key_press_type_t press_type;        /* 本次事件的按压类型。 */
} key_event_t;

/*****************************************************************************
@brief: 初始化按键服务
@para:cfg 产品按键绑定和扫描时序配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int key_service_init(const key_service_cfg_t *cfg);

/*****************************************************************************
@brief: 更新按键按压时序阈值
@para:debounce_ms 消抖确认时间，单位 ms
@para:long_press_ms 长按阈值，单位 ms
@para:very_long_press_ms 超长按阈值，单位 ms
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int key_service_set_timing(uint32_t debounce_ms, uint32_t long_press_ms, uint32_t very_long_press_ms);

/*****************************************************************************
@brief: 获取一个待处理的按键事件
@para:event 输出参数，用于保存按键事件
@return: RET_OK 表示成功获取事件，
         RET_NOT_FOUND 表示当前没有待处理事件，
         其他返回值表示失败
*******************************************************************************/
int key_service_get_event(key_event_t *event, uint32_t timeout_ms);

#endif
