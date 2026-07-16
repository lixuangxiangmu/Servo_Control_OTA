#ifndef REMOTE_SERVICE_H
#define REMOTE_SERVICE_H

#include <stdint.h>

/* 遥控器逻辑 ID 定义，上层只使用逻辑 ID，不关心具体 GPIO 或 TIMER 资源。 */
typedef enum
{
    REMOTE_MAIN = 0,       /* 主 433MHz 遥控器接收通道。 */
    REMOTE_ID_MAX,         /* 遥控器数量边界值，不作为真实遥控器使用。 */
} remote_id_t;

/* 433MHz 遥控器服务配置，由 application/app_config.c 注入。 */
typedef struct
{
    const char *rf_gpio_name;          /* 433MHz 接收模块数据 GPIO 设备名，例如 "remote_rf_data"。 */
    const char *sample_timer_name;     /* 采样用周期定时器设备名，例如 "remote_sample_timer"。 */
    uint32_t sample_period_us;         /* 采样周期，单位 us；EV1527 默认使用 100us。 */
    uint32_t pair_timeout_ms;          /* 两帧一致校验的最大等待时间，单位 ms。 */
    uint32_t repeat_suppress_ms;       /* 同一个键码重复上报抑制时间，单位 ms；0 表示不抑制。 */
    uint8_t event_queue_len;           /* 遥控器事件队列深度，队列满时新事件会被丢弃。 */
    uint8_t auto_start;                /* 初始化后是否自动启动采样定时器，非 0 表示自动启动。 */
} remote_service_cfg_t;

/* 遥控器服务向应用层上报的事件。 */
typedef struct
{
    remote_id_t remote_id;             /* 触发事件的遥控器逻辑 ID。 */
    uint32_t raw_code;                 /* EV1527 24 位原始码，低 24 位有效。 */
    uint32_t address_code;             /* EV1527 地址码，通常为高 20 位。 */
    uint8_t key_code;                  /* EV1527 按键码，通常为低 4 位。 */
    uint8_t data[3];                   /* 按老工程字节顺序拆出的 3 字节数据：data[0..1] 为特征码，data[2] 为命令码。 */
} remote_event_t;

/*****************************************************************************
@brief: 初始化 433MHz 遥控器服务
@para:cfg 产品层注入的 GPIO、定时器和事件队列配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
@note: 服务层只解析 EV1527 脉宽协议并上报事件，不直接执行舵机动作或存储对码结果。
*******************************************************************************/
int remote_service_init(const remote_service_cfg_t *cfg);

/*****************************************************************************
@brief: 启动 433MHz 遥控器采样
@para:
@return: RET_OK 表示启动成功，其他返回值表示失败
*******************************************************************************/
int remote_service_start(void);

/*****************************************************************************
@brief: 停止 433MHz 遥控器采样
@para:
@return: RET_OK 表示停止成功，其他返回值表示失败
*******************************************************************************/
int remote_service_stop(void);

/*****************************************************************************
@brief: 获取一个待处理的遥控器事件
@para:event 输出参数，用于保存遥控器事件
@para:timeout_ms 队列等待时间，当前项目沿用 FreeRTOS tick 风格传参
@return: RET_OK 表示成功读取到事件，RET_NOT_FOUND 表示超时或当前没有事件
*******************************************************************************/
int remote_service_get_event(remote_event_t *event, uint32_t timeout_ms);

#endif
