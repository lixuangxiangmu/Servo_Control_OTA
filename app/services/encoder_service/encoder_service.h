#ifndef ENCODER_SERVICE_H
#define ENCODER_SERVICE_H

#include <stdint.h>

typedef enum
{
    ENCODER_MAIN = 0,       /* 主旋钮编码器。当前项目只有一个 EC11，因此先保留一个逻辑 ID。 */
    ENCODER_ID_MAX,         /* 编码器数量边界值，不作为真实编码器使用。 */
} encoder_id_t;

/* 编码器旋转方向。
   CW/CCW 是服务层输出的逻辑方向，实际方向如果和结构件安装相反，可通过 reverse 配置反转。 */
typedef enum
{
    ENCODER_ROTATE_NONE = 0,    /* 无有效旋转事件。 */
    ENCODER_ROTATE_CW,          /* 顺时针旋转事件。 */
    ENCODER_ROTATE_CCW,         /* 逆时针旋转事件。 */
} encoder_rotate_t;

/* 单个编码器的硬件绑定配置。
   该配置由 application/app_config.c 注入，服务层不直接依赖 board_config.h。 */
typedef struct
{
    const char *phase_a_gpio_name;   /* A 相 GPIO 设备名，例如 "encoder_phase_a"。 */
    const char *phase_b_gpio_name;   /* B 相 GPIO 设备名，例如 "encoder_phase_b"。 */
    uint8_t reverse;                 /* 方向反转开关：0 保持默认判向，非 0 交换 CW/CCW。 */
    uint32_t debounce_ms;            /* 有效旋转事件之间的最小间隔，单位 ms；0 表示不做软件消抖。 */
} encoder_service_item_cfg_t;

/* 编码器服务整体配置。 */
typedef struct
{
    const encoder_service_item_cfg_t *items;    /* 编码器配置表。 */
    uint8_t item_count;                         /* 编码器数量，不能超过 ENCODER_ID_MAX。 */
    uint8_t event_queue_len;                    /* 旋转事件队列深度，队列满时新事件会被丢弃。 */
} encoder_service_cfg_t;

/* 编码器服务向应用层上报的事件。 */
typedef struct
{
    encoder_id_t encoder_id;        /* 触发事件的编码器逻辑 ID。 */
    encoder_rotate_t rotate;        /* 本次旋转方向。 */
} encoder_event_t;

/*****************************************************************************
@brief: 初始化编码器服务
@para:cfg 产品层注入的编码器 GPIO 绑定、方向和消抖配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
@note: 初始化过程中会为 A/B 两相 GPIO 绑定下降沿中断。服务层只产生旋转事件，
       不直接修改舵机速度、角度或显示状态。
*******************************************************************************/
int encoder_service_init(const encoder_service_cfg_t *cfg);

/*****************************************************************************
@brief: 获取一个待处理的编码器旋转事件
@para:event 输出参数，用于保存读取到的旋转事件
@para:timeout_ms 队列等待时间，当前项目沿用 FreeRTOS tick 风格传参
@return: RET_OK 表示成功读取到事件，RET_NOT_FOUND 表示超时或当前没有事件
@note: 应用层可在任务中调用该函数，根据 CW/CCW 决定具体业务动作。
*******************************************************************************/
int encoder_service_get_event(encoder_event_t *event, uint32_t timeout_ms);

/*****************************************************************************
@brief: 运行时设置指定编码器的方向反转
@para:encoder_id 编码器逻辑 ID
@para:reverse 0 表示保持默认方向，非 0 表示交换 CW/CCW
@return: RET_OK 表示设置成功，其他返回值表示失败
@note: 该接口用于实测发现旋钮方向与产品预期相反时快速修正。
*******************************************************************************/
int encoder_service_set_reverse(encoder_id_t encoder_id, uint8_t reverse);

#endif
