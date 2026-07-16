#include "remote_service.h"

#include "dev_gpio.h"
#include "dev_timer.h"
#include "return_code.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdint.h>

/* EV1527 解码需要的采样窗口，单位为 100us 采样 tick。 */
#define REMOTE_SYNC_HIGH_MIN_TICKS       2U
#define REMOTE_SYNC_HIGH_MAX_TICKS       5U
#define REMOTE_SYNC_LOW_MIN_TICKS        100U
#define REMOTE_SYNC_LOW_MAX_TICKS        130U
#define REMOTE_BIT_ONE_LOW_MIN_TICKS     2U
#define REMOTE_BIT_ONE_LOW_MAX_TICKS     7U
#define REMOTE_BIT_ZERO_LOW_MIN_TICKS    8U
#define REMOTE_BIT_ZERO_LOW_MAX_TICKS    13U
#define REMOTE_EV1527_BIT_COUNT          24U
#define REMOTE_COUNTER_SATURATE          0xFFFFU

/* 433MHz 遥控器解码运行时状态。
   这些字段会在定时器 ISR 回调中更新，用于记录高低电平宽度、同步状态和双帧校验状态。 */
typedef struct
{
    uint16_t high_ticks;             /* 最近一个高电平段宽度，单位为采样 tick。 */
    uint16_t low_ticks;              /* 最近一个低电平段宽度，单位为采样 tick。 */
    uint8_t last_is_high;            /* 上一次采样是否为高电平，用于识别低到高跳变。 */
    uint8_t sync_ok;                 /* 是否已经识别到 EV1527 同步码。 */
    uint8_t bit_count;               /* 当前帧已接收 bit 数。 */
    uint32_t frame_code;             /* 当前正在移入的 24 位帧数据。 */
    uint8_t first_frame_pending;     /* 是否已经收到第一帧，正在等待第二帧确认。 */
    uint32_t first_frame_code;       /* 第一帧候选码，用于和第二帧比较。 */
    uint32_t first_frame_timeout_ticks; /* 第一帧等待第二帧的剩余超时 tick。 */
    uint32_t repeat_suppress_ticks;  /* 同码重复上报抑制剩余 tick。 */
    uint32_t last_report_code;       /* 上一次已经上报的有效码。 */
} remote_decode_runtime_t;

/* 遥控器服务运行时对象。
   该结构体集中保存应用注入配置、事件队列和 ISR 解码状态，避免使用分散的全局变量。 */
typedef struct
{
    remote_service_cfg_t cfg;          /* 应用层注入的遥控器服务配置副本。 */
    QueueHandle_t remote_event_queue;         /* 遥控器事件队列，ISR 成功解码后向该队列投递事件。 */
    remote_decode_runtime_t decoder;   /* EV1527 解码运行时状态。 */
    uint32_t pair_timeout_ticks;        /* 两帧一致校验超时时间，单位为采样 tick。 */
    uint32_t repeat_suppress_reload;    /* 重复上报抑制重载值，单位为采样 tick。 */
    uint8_t inited;                     /* 服务是否已经完成初始化。 */
} remote_service_runtime_t;

/* 当前项目只有一个 433MHz 遥控器接收通道，因此使用一个静态运行时对象。 */
static remote_service_runtime_t s_remote;

/*****************************************************************************
@brief: 判断一个计数值是否落在指定窗口内
@para:value 待判断的计数值
@para:min_value 窗口最小值
@para:max_value 窗口最大值
@return: 非 0 表示在窗口内，0 表示不在窗口内
*******************************************************************************/
static uint8_t remote_value_in_range(uint16_t value, uint16_t min_value, uint16_t max_value)
{
    return ((value >= min_value) && (value <= max_value)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 将毫秒时间转换为采样 tick 数
@para:ms 毫秒时间
@para:sample_period_us 采样周期，单位 us
@return: 转换后的采样 tick 数，输入非 0 时至少返回 1
*******************************************************************************/
static uint32_t remote_ms_to_sample_ticks(uint32_t ms, uint32_t sample_period_us)
{
    uint32_t ticks;

    if ((ms == 0U) || (sample_period_us == 0U))
    {
        return 0U;
    }

    ticks = ((ms * 1000U) + sample_period_us - 1U) / sample_period_us;

    return (ticks == 0U) ? 1U : ticks;
}

/*****************************************************************************
@brief: 复位当前帧解码状态
@para:decoder EV1527 解码状态指针
@return:
*******************************************************************************/
static void remote_reset_frame(remote_decode_runtime_t *decoder)
{
    if (decoder == 0)
    {
        return;
    }

    decoder->sync_ok = 0U;
    decoder->bit_count = 0U;
    decoder->frame_code = 0U;
}

/*****************************************************************************
@brief: 清空 EV1527 解码器全部运行状态
@para:decoder EV1527 解码状态指针
@return:
*******************************************************************************/
static void remote_reset_decoder(remote_decode_runtime_t *decoder)
{
    if (decoder == 0)
    {
        return;
    }

    decoder->high_ticks = 0U;
    decoder->low_ticks = 0U;
    decoder->last_is_high = 1U;
    decoder->first_frame_pending = 0U;
    decoder->first_frame_code = 0U;
    decoder->first_frame_timeout_ticks = 0U;
    decoder->repeat_suppress_ticks = 0U;
    decoder->last_report_code = 0U;
    remote_reset_frame(decoder);
}

/*****************************************************************************
@brief: 对 16 位计数器做饱和加 1
@para:value 需要递增的计数器指针
@return:
*******************************************************************************/
static void remote_counter_inc(uint16_t *value)
{
    if ((value != 0) && (*value < REMOTE_COUNTER_SATURATE))
    {
        (*value)++;
    }
}

/*****************************************************************************
@brief: 将 24 位原始码转换为遥控器事件结构
@para:raw_code EV1527 24 位原始码
@para:event 输出参数，用于保存事件内容
@return:
*******************************************************************************/
static void remote_fill_event(uint32_t raw_code, remote_event_t *event)
{
    if (event == 0)
    {
        return;
    }

    raw_code &= 0x00FFFFFFUL;
    event->remote_id = REMOTE_MAIN;
    event->raw_code = raw_code;
    event->address_code = (raw_code >> 4) & 0x000FFFFFUL;
    event->key_code = (uint8_t)(raw_code & 0x0FU);
    event->data[0] = (uint8_t)((raw_code >> 16) & 0xFFU);
    event->data[1] = (uint8_t)((raw_code >> 8) & 0xFFU);
    event->data[2] = (uint8_t)(raw_code & 0xFFU);
}

/*****************************************************************************
@brief: 在 ISR 中投递一个遥控器事件
@para:raw_code EV1527 24 位原始码
@return:
@note: 队列满时直接丢弃新事件，避免 ISR 阻塞。
*******************************************************************************/
static void remote_push_event_from_isr(uint32_t raw_code)
{
    remote_event_t event;
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((s_remote.remote_event_queue == 0) || (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED))
    {
        return;
    }

    remote_fill_event(raw_code, &event);
    (void)xQueueSendFromISR(s_remote.remote_event_queue, &event, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/*****************************************************************************
@brief: 处理一帧完整 EV1527 数据
@para:raw_code EV1527 24 位原始码
@return:
@note: 沿用老工程“两帧相同才有效”的策略，降低 433MHz 干扰造成的误触发。
*******************************************************************************/
static void remote_handle_complete_frame(uint32_t raw_code)
{
    remote_decode_runtime_t *decoder = &s_remote.decoder;

    raw_code &= 0x00FFFFFFUL;

    if (decoder->first_frame_pending == 0U)
    {
        decoder->first_frame_code = raw_code;
        decoder->first_frame_timeout_ticks = s_remote.pair_timeout_ticks;
        decoder->first_frame_pending = 1U;
        return;
    }

    if (decoder->first_frame_code == raw_code)
    {
        if (!((decoder->repeat_suppress_ticks != 0U) && (decoder->last_report_code == raw_code)))
        {
            remote_push_event_from_isr(raw_code);
            decoder->last_report_code = raw_code;
            decoder->repeat_suppress_ticks = s_remote.repeat_suppress_reload;
        }
    }

    decoder->first_frame_pending = 0U;
    decoder->first_frame_timeout_ticks = 0U;
}

/*****************************************************************************
@brief: 向当前帧追加一个 EV1527 数据 bit
@para:bit_value 当前 bit 值，0 或 1
@return:
*******************************************************************************/
static void remote_append_bit(uint8_t bit_value)
{
    remote_decode_runtime_t *decoder = &s_remote.decoder;

    if (decoder->sync_ok == 0U)
    {
        return;
    }

    decoder->frame_code = ((decoder->frame_code << 1) | ((uint32_t)bit_value & 0x01U)) & 0x00FFFFFFUL;
    decoder->bit_count++;

    if (decoder->bit_count >= REMOTE_EV1527_BIT_COUNT)
    {
        remote_handle_complete_frame(decoder->frame_code);
        remote_reset_frame(decoder);
    }
}

/*****************************************************************************
@brief: 处理一次低电平到高电平跳变
@para:
@return:
@note: EV1527 的同步码和数据位都在低电平结束后的上升沿处判定。
*******************************************************************************/
static void remote_process_rising_edge(void)
{
    remote_decode_runtime_t *decoder = &s_remote.decoder;

    if ((remote_value_in_range(decoder->high_ticks, REMOTE_SYNC_HIGH_MIN_TICKS, REMOTE_SYNC_HIGH_MAX_TICKS) != 0U) &&
        (remote_value_in_range(decoder->low_ticks, REMOTE_SYNC_LOW_MIN_TICKS, REMOTE_SYNC_LOW_MAX_TICKS) != 0U))
    {
        decoder->sync_ok = 1U;
        decoder->bit_count = 0U;
        decoder->frame_code = 0U;
    }
    else if ((decoder->sync_ok != 0U) &&
             (remote_value_in_range(decoder->low_ticks, REMOTE_BIT_ONE_LOW_MIN_TICKS, REMOTE_BIT_ONE_LOW_MAX_TICKS) != 0U))
    {
        remote_append_bit(1U);
    }
    else if ((decoder->sync_ok != 0U) &&
             (remote_value_in_range(decoder->low_ticks, REMOTE_BIT_ZERO_LOW_MIN_TICKS, REMOTE_BIT_ZERO_LOW_MAX_TICKS) != 0U))
    {
        remote_append_bit(0U);
    }
    else
    {
        remote_reset_frame(decoder);
    }

    decoder->low_ticks = 0U;
    decoder->high_ticks = 1U;
}

/*****************************************************************************
@brief: 维护双帧校验和重复上报抑制的倒计时
@para:
@return:
*******************************************************************************/
static void remote_update_timers(void)
{
    remote_decode_runtime_t *decoder = &s_remote.decoder;

    if (decoder->first_frame_pending != 0U)
    {
        if (decoder->first_frame_timeout_ticks > 0U)
        {
            decoder->first_frame_timeout_ticks--;
        }
        else
        {
            decoder->first_frame_pending = 0U;
        }
    }

    if (decoder->repeat_suppress_ticks > 0U)
    {
        decoder->repeat_suppress_ticks--;
    }
}

/*****************************************************************************
@brief: 周期定时器 ISR 回调，完成一次 433MHz 输入采样
@para:name 触发回调的定时器设备名，当前服务只有一个定时器，因此不使用
@para:user_ctx 用户上下文，当前服务未使用
@return:
*******************************************************************************/
static void remote_timer_callback(const char *name, void *user_ctx)
{
    gpio_level_t level;
    uint8_t is_high;

    (void)name;
    (void)user_ctx;

    if ((s_remote.inited == 0U) || (dev_gpio_read(s_remote.cfg.rf_gpio_name, &level) != RET_OK))
    {
        return;
    }

    is_high = (level == GPIO_LEVEL_HIGH) ? 1U : 0U;
    if (is_high == 0U)
    {
        remote_counter_inc(&s_remote.decoder.low_ticks);
        s_remote.decoder.last_is_high = 0U;
    }
    else
    {
        remote_counter_inc(&s_remote.decoder.high_ticks);
        if (s_remote.decoder.last_is_high == 0U)
        {
            remote_process_rising_edge();
        }
        s_remote.decoder.last_is_high = 1U;
    }

    remote_update_timers();
}

/*****************************************************************************
@brief: 初始化 433MHz 遥控器服务
@para:cfg 产品层注入的 GPIO、定时器和事件队列配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int remote_service_init(const remote_service_cfg_t *cfg)
{
    timer_config_t timer_cfg;
    int ret;

    if ((cfg == 0) || (cfg->rf_gpio_name == 0) || (cfg->sample_timer_name == 0) ||
        (cfg->sample_period_us == 0U) || (cfg->pair_timeout_ms == 0U) ||
        (cfg->event_queue_len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_remote.cfg = *cfg;
    s_remote.pair_timeout_ticks = remote_ms_to_sample_ticks(cfg->pair_timeout_ms, cfg->sample_period_us);
    s_remote.repeat_suppress_reload = remote_ms_to_sample_ticks(cfg->repeat_suppress_ms, cfg->sample_period_us);
    remote_reset_decoder(&s_remote.decoder);

    s_remote.remote_event_queue = xQueueCreate((UBaseType_t)cfg->event_queue_len, sizeof(remote_event_t));
    if (s_remote.remote_event_queue == 0)
    {
        return RET_NO_RESOURCE;
    }

    timer_cfg.period_us = cfg->sample_period_us;
    ret = dev_timer_config(cfg->sample_timer_name, &timer_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = dev_timer_attach_callback(cfg->sample_timer_name, remote_timer_callback, 0);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    s_remote.inited = 1U;

    if (cfg->auto_start != 0U)
    {
        ret = remote_service_start();
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 启动 433MHz 遥控器采样
@para:
@return: RET_OK 表示启动成功，其他返回值表示失败
*******************************************************************************/
int remote_service_start(void)
{
    if (s_remote.inited == 0U)
    {
        return RET_NOT_INITED;
    }

    remote_reset_decoder(&s_remote.decoder);

    return dev_timer_start(s_remote.cfg.sample_timer_name);
}

/*****************************************************************************
@brief: 停止 433MHz 遥控器采样
@para:
@return: RET_OK 表示停止成功，其他返回值表示失败
*******************************************************************************/
int remote_service_stop(void)
{
    if (s_remote.inited == 0U)
    {
        return RET_NOT_INITED;
    }

    return dev_timer_stop(s_remote.cfg.sample_timer_name);
}

/*****************************************************************************
@brief: 获取一个待处理的遥控器事件
@para:event 输出参数，用于保存遥控器事件
@para:timeout_ms 队列等待时间，当前项目沿用 FreeRTOS tick 风格传参
@return: RET_OK 表示成功读取到事件，RET_NOT_FOUND 表示超时或当前没有事件
*******************************************************************************/
int remote_service_get_event(remote_event_t *event, uint32_t timeout_ms)
{
    if (event == 0)
    {
        return RET_INVALID_PARAM;
    }

    if ((s_remote.inited == 0U) || (s_remote.remote_event_queue == 0))
    {
        return RET_NOT_INITED;
    }

    if (xQueueReceive(s_remote.remote_event_queue, event, (TickType_t)timeout_ms) != pdTRUE)
    {
        return RET_NOT_FOUND;
    }

    return RET_OK;
}
