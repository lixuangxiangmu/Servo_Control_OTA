#ifndef DEV_TIMER_H
#define DEV_TIMER_H

#include <stdint.h>

/* 定时器周期到达回调函数类型。
   该回调由具体芯片驱动在硬件定时器中断上下文中调用，回调内部只能执行轻量逻辑，
   如果需要通知任务，必须使用 FreeRTOS FromISR 系列接口。 */
typedef void (*timer_period_elapsed_cb_t)(const char *name, void *user_ctx);

/* 通用周期定时器运行配置。
   该结构体不包含具体芯片寄存器、时钟枚举或中断号，只表达上层需要的周期语义。 */
typedef struct
{
    uint32_t period_us;      /* 定时器更新周期，单位 us。 */
} timer_config_t;

/* 定时器回调绑定配置。
   dev_timer_attach_callback() 会把该结构体传给底层驱动保存，底层只负责在周期中断中回调。 */
typedef struct
{
    timer_period_elapsed_cb_t cb;    /* 周期到达回调函数。 */
    void *user_ctx;                  /* 用户上下文指针，回调时原样传回。 */
} timer_callback_config_t;

/* TIMER 设备控制命令定义，用于 device_control 分发到底层驱动。 */
#define DEV_TIMER_CMD_CONFIG              1
#define DEV_TIMER_CMD_ATTACH_CALLBACK     2
#define DEV_TIMER_CMD_DETACH_CALLBACK     3

/*****************************************************************************
@brief: 配置周期定时器参数
@para:name 定时器设备名称
@para:cfg 定时器周期配置
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
int dev_timer_config(const char *name, const timer_config_t *cfg);

/*****************************************************************************
@brief: 绑定周期定时器中断回调
@para:name 定时器设备名称
@para:cb 周期到达回调函数，运行在 ISR 上下文中
@para:user_ctx 用户上下文指针，回调时原样传回
@return: RET_OK 表示绑定成功，其他返回值表示失败
*******************************************************************************/
int dev_timer_attach_callback(const char *name, timer_period_elapsed_cb_t cb, void *user_ctx);

/*****************************************************************************
@brief: 解绑周期定时器中断回调
@para:name 定时器设备名称
@return: RET_OK 表示解绑成功，其他返回值表示失败
*******************************************************************************/
int dev_timer_detach_callback(const char *name);

/*****************************************************************************
@brief: 启动周期定时器
@para:name 定时器设备名称
@return: RET_OK 表示启动成功，其他返回值表示失败
*******************************************************************************/
int dev_timer_start(const char *name);

/*****************************************************************************
@brief: 停止周期定时器
@para:name 定时器设备名称
@return: RET_OK 表示停止成功，其他返回值表示失败
*******************************************************************************/
int dev_timer_stop(const char *name);

#endif
