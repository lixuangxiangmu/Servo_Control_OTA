#include "encoder_service.h"

#include "dev_gpio.h"
#include "return_code.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <stdint.h>

#define ENCODER_PHASE_A 0U
#define ENCODER_PHASE_B 1U

/* GPIO 中断回调上下文。
   同一个回调函数同时服务 A/B 两相，需要通过该上下文知道：
   1. 是哪个编码器触发；
   2. 是 A 相还是 B 相触发。 */
typedef struct
{
    encoder_id_t encoder_id;        /* 编码器逻辑 ID。 */
    uint8_t phase;                  /* 触发中断的相位，取 ENCODER_PHASE_A/B。 */
} encoder_irq_ctx_t;

/* 单个编码器运行时状态。这些状态不放在配置结构里，是因为它们会在中断回调中动态更新。 */
typedef struct
{
    TickType_t last_event_tick;     /* 上一次有效旋转事件的 tick，用于软件消抖。 */
    uint8_t has_event;              /* 是否已经产生过有效事件，避免 tick 为 0 时被误判。 */
    uint8_t reverse;                /* 当前方向反转状态，可由配置或运行时接口设置。 */
} encoder_runtime_t;

/* 应用层注入的编码器配置表，服务层只保存指针，不复制整张表。 */
static const encoder_service_item_cfg_t *s_encoder_items;
static uint8_t s_encoder_item_count;

/* 每个编码器一份运行时状态。 */
static encoder_runtime_t s_encoder_runtime[ENCODER_ID_MAX];

/* 每个编码器的 A/B 两相各自一份中断上下文，供 GPIO 回调区分来源。 */
static encoder_irq_ctx_t s_encoder_irq_ctx[ENCODER_ID_MAX][2];

/* 编码器旋转事件队列，中断里只把 CW/CCW 事件投递到队列，具体业务由应用任务消费。 */
static QueueHandle_t s_encoder_event_queue;

/*****************************************************************************
@brief: 检查编码器逻辑 ID 是否有效且已经完成配置
@para:encoder_id 编码器逻辑 ID
@return: 1 表示有效，0 表示无效
*******************************************************************************/
static uint8_t encoder_id_is_valid(encoder_id_t encoder_id)
{
    return ((encoder_id < ENCODER_ID_MAX) &&
            (s_encoder_items != 0) &&
            ((uint8_t)encoder_id < s_encoder_item_count) &&
            (s_encoder_items[encoder_id].phase_a_gpio_name != 0) &&
            (s_encoder_items[encoder_id].phase_b_gpio_name != 0)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 根据 reverse 状态修正旋转方向
@para:encoder_id 编码器逻辑 ID
@para:rotate 原始判向结果
@return: 可能被反转后的方向
@note: 机械安装方向、旋钮丝印方向或 A/B 接线顺序不同，都可能导致方向相反。
       这里集中处理反转，上层业务无需关心硬件差异。
*******************************************************************************/
static encoder_rotate_t encoder_apply_reverse(encoder_id_t encoder_id, encoder_rotate_t rotate)
{
    if ((rotate == ENCODER_ROTATE_CW) && (s_encoder_runtime[encoder_id].reverse != 0U))
    {
        return ENCODER_ROTATE_CCW;
    }

    if ((rotate == ENCODER_ROTATE_CCW) && (s_encoder_runtime[encoder_id].reverse != 0U))
    {
        return ENCODER_ROTATE_CW;
    }

    return rotate;
}

/*****************************************************************************
@brief: 判断本次有效旋转事件是否通过软件消抖
@para:encoder_id 编码器逻辑 ID
@return: 1 表示允许上报，0 表示距离上次有效事件太近，需要丢弃
@note: 该函数在 GPIO 中断回调中调用，因此使用 xTaskGetTickCountFromISR()。
       消抖只作用于已经判定为 CW/CCW 的有效旋转事件，避免无效抖动边沿刷新时间戳。
*******************************************************************************/
static uint8_t encoder_debounce_passed(encoder_id_t encoder_id)
{
    TickType_t now_tick;
    uint32_t elapsed_ms;
    uint32_t debounce_ms;

    debounce_ms = s_encoder_items[encoder_id].debounce_ms;
    if (debounce_ms == 0U)
    {
        return 1U;
    }

    now_tick = xTaskGetTickCountFromISR();
    /* 第一次有效事件没有历史时间可比较，直接放行并记录时间。 */
    if (s_encoder_runtime[encoder_id].has_event == 0U)
    {
        s_encoder_runtime[encoder_id].last_event_tick = now_tick;
        s_encoder_runtime[encoder_id].has_event = 1U;
        return 1U;
    }

    elapsed_ms = (uint32_t)((now_tick - s_encoder_runtime[encoder_id].last_event_tick) * portTICK_PERIOD_MS);
    if (elapsed_ms < debounce_ms)
    {
        return 0U;
    }

    s_encoder_runtime[encoder_id].last_event_tick = now_tick;

    return 1U;
}

/*****************************************************************************
@brief: 在 ISR 中把编码器事件压入队列
@para:encoder_id 编码器逻辑 ID
@para:rotate 旋转方向
@return:
@note: 该函数运行在 EXTI ISR 回调链路中，只能使用 FromISR API。
       如果队列已满，xQueueSendFromISR 会失败；这里不阻塞、不重试，保证 ISR 足够短。
*******************************************************************************/
static void encoder_push_event_from_isr(encoder_id_t encoder_id, encoder_rotate_t rotate)
{
    encoder_event_t event;
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((s_encoder_event_queue == 0) || (rotate == ENCODER_ROTATE_NONE))
    {
        return;
    }

    event.encoder_id = encoder_id;
    event.rotate = rotate;

    (void)xQueueSendFromISR(s_encoder_event_queue, &event, &higher_priority_task_woken);
    portYIELD_FROM_ISR(higher_priority_task_woken);
}

/*****************************************************************************
@brief: EC11 A/B 相 GPIO 中断回调
@para:name 触发中断的 GPIO 设备名，本服务通过 user_ctx 区分来源，因此这里不使用
@para:event GPIO 边沿事件，EC11 当前只处理下降沿
@para:user_ctx encoder_irq_ctx_t 指针，标识编码器 ID 和触发相位
@return:
@note: 判向逻辑沿用老工程思路：
       - A 相下降沿时，如果 B 相为低电平，则认为是一个方向；
       - B 相下降沿时，如果 A 相为低电平，则认为是另一个方向。
       默认映射为 A 相下降沿 -> CCW，B 相下降沿 -> CW，可通过 reverse 反转。
*******************************************************************************/
static void encoder_gpio_irq_cb(const char *name, gpio_irq_event_t event, void *user_ctx)
{
    const encoder_irq_ctx_t *ctx;
    const encoder_service_item_cfg_t *item;
    gpio_level_t other_level;
    encoder_rotate_t rotate = ENCODER_ROTATE_NONE;

    (void)name;

    if ((event != GPIO_IRQ_EVENT_FALLING) || (user_ctx == 0))
    {
        return;
    }

    ctx = (const encoder_irq_ctx_t *)user_ctx;
    if (encoder_id_is_valid(ctx->encoder_id) == 0U)
    {
        return;
    }

    item = &s_encoder_items[ctx->encoder_id];

    if (ctx->phase == ENCODER_PHASE_A)
    {
        /* A 相下降沿到来后读取 B 相电平，利用两相信号相位差判断方向。 */
        if (dev_gpio_read(item->phase_b_gpio_name, &other_level) != RET_OK)
        {
            return;
        }

        if (other_level == GPIO_LEVEL_LOW)
        {
            rotate = ENCODER_ROTATE_CCW;
        }
    }
    else
    {
        /* B 相下降沿到来后读取 A 相电平，和 A 相分支形成相反方向。 */
        if (dev_gpio_read(item->phase_a_gpio_name, &other_level) != RET_OK)
        {
            return;
        }

        if (other_level == GPIO_LEVEL_LOW)
        {
            rotate = ENCODER_ROTATE_CW;
        }
    }

    /* 只有形成了有效方向后才做消抖判断，避免无效边沿刷新 last_event_tick。 */
    if ((rotate == ENCODER_ROTATE_NONE) || (encoder_debounce_passed(ctx->encoder_id) == 0U))
    {
        return;
    }

    encoder_push_event_from_isr(ctx->encoder_id, encoder_apply_reverse(ctx->encoder_id, rotate));
}

/*****************************************************************************
@brief: 初始化编码器服务
@para:cfg 产品层注入的编码器配置
@return: RET_OK 表示成功，其他返回值表示失败
*******************************************************************************/
int encoder_service_init(const encoder_service_cfg_t *cfg)
{
    uint8_t i;
    int ret;

    if ((cfg == 0) || (cfg->items == 0) || (cfg->item_count == 0U) ||
        (cfg->item_count > (uint8_t)ENCODER_ID_MAX) || (cfg->event_queue_len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_encoder_items = cfg->items;               //编码器配置表
    s_encoder_item_count = cfg->item_count;     //编码器数量

    /* 用队列缓存旋转事件，让 ISR 和应用任务解耦。ISR 只负责投递事件，应用任务决定旋转后要修改哪个业务参数。 */
    s_encoder_event_queue = xQueueCreate((UBaseType_t)cfg->event_queue_len, sizeof(encoder_event_t));
    if (s_encoder_event_queue == 0)
    {
        return RET_NO_RESOURCE;
    }

    for (i = 0U; i < s_encoder_item_count; i++)
    {
        if ((s_encoder_items[i].phase_a_gpio_name == 0) || (s_encoder_items[i].phase_b_gpio_name == 0))
        {
            return RET_INVALID_PARAM;
        }

        s_encoder_runtime[i].last_event_tick = 0U;
        s_encoder_runtime[i].has_event = 0U;
        s_encoder_runtime[i].reverse = (s_encoder_items[i].reverse != 0U) ? 1U : 0U;

        s_encoder_irq_ctx[i][ENCODER_PHASE_A].encoder_id = (encoder_id_t)i;
        s_encoder_irq_ctx[i][ENCODER_PHASE_A].phase = ENCODER_PHASE_A;
        s_encoder_irq_ctx[i][ENCODER_PHASE_B].encoder_id = (encoder_id_t)i;
        s_encoder_irq_ctx[i][ENCODER_PHASE_B].phase = ENCODER_PHASE_B;

        /* A/B 两相都使用下降沿中断，和老工程 PB8/PB9 falling edge 方案保持一致。 */
        ret = dev_gpio_attach_irq(s_encoder_items[i].phase_a_gpio_name,
                                  GPIO_IRQ_TRIGGER_FALLING,
                                  encoder_gpio_irq_cb,
                                  &s_encoder_irq_ctx[i][ENCODER_PHASE_A]);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = dev_gpio_attach_irq(s_encoder_items[i].phase_b_gpio_name,
                                  GPIO_IRQ_TRIGGER_FALLING,
                                  encoder_gpio_irq_cb,
                                  &s_encoder_irq_ctx[i][ENCODER_PHASE_B]);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 获取一个编码器旋转事件
@para:event 输出事件
@para:timeout_ms 等待时间，当前保持和 key_service 一致，直接传给 xQueueReceive
@return: RET_OK 表示读到事件，RET_NOT_FOUND 表示无事件或超时
*******************************************************************************/
int encoder_service_get_event(encoder_event_t *event, uint32_t timeout_ms)
{
    if (event == 0)
    {
        return RET_INVALID_PARAM;
    }

    if (s_encoder_event_queue == 0)
    {
        return RET_NOT_INITED;
    }

    if (xQueueReceive(s_encoder_event_queue, event, (TickType_t)timeout_ms) != pdTRUE)
    {
        return RET_NOT_FOUND;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 设置编码器方向反转
@para:encoder_id 编码器逻辑 ID
@para:reverse 0 表示不反转，非 0 表示反转
@return: RET_OK 表示成功，其他返回值表示失败
*******************************************************************************/
int encoder_service_set_reverse(encoder_id_t encoder_id, uint8_t reverse)
{
    if (encoder_id_is_valid(encoder_id) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    s_encoder_runtime[encoder_id].reverse = (reverse != 0U) ? 1U : 0U;

    return RET_OK;
}
