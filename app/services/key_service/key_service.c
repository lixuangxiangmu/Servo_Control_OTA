#include "key_service.h"

#include "return_code.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "timers.h"

/* 按键事件队列长度。队列满时新事件会被丢弃，避免定时器回调阻塞。 */
#define KEY_SERVICE_EVENT_QUEUE_LEN 8U

/* 按键内部消抖状态机定义。 */
typedef enum
{
    KEY_SCAN_RELEASE = 0,          /* 松开状态，等待检测到按下。 */
    KEY_SCAN_CONFIRM,              /* 按下确认状态，用于消抖。 */
    KEY_SCAN_SHORT_PRESS,          /* 已确认按下，释放时如果未达到长按则上报短按。 */
    KEY_SCAN_LONG_PRESS,           /* 已上报长按，继续按住可升级为超长按。 */
    KEY_SCAN_VERY_LONG_PRESS,      /* 已上报超长按，等待释放复位。 */
} key_scan_state_t;

/* 单个按键运行态信息。 */
typedef struct
{
    key_scan_state_t state;        /* 当前按键扫描状态。 */
    TickType_t press_tick;         /* 本次有效按下开始时刻，单位为 FreeRTOS tick。 */
} key_runtime_t;

/* 应用层注入的按键硬件绑定表。 */
static const key_service_item_cfg_t *s_key_items;

/* 按键硬件绑定数量。 */
static uint8_t s_key_item_count;

/* 按键扫描时序阈值。 */
static uint32_t s_key_debounce_ms;
static uint32_t s_key_long_press_ms;
static uint32_t s_key_very_long_press_ms;

/* 每个按键对应一份独立状态机运行态。 */
static key_runtime_t s_key_runtime[KEY_ID_MAX];

/* 按键事件队列和周期扫描软件定时器。 */
static QueueHandle_t s_key_event_queue;
static TimerHandle_t s_key_timer_handle;

/*****************************************************************************
@brief: 检查按键时序配置是否合法
@para:debounce_ms 消抖确认时间
@para:long_press_ms 长按阈值
@para:very_long_press_ms 超长按阈值
@return: 1 表示合法，0 表示非法
*******************************************************************************/
static uint8_t key_timing_is_valid(uint32_t debounce_ms,
                                   uint32_t long_press_ms,
                                   uint32_t very_long_press_ms)
{
    return ((debounce_ms > 0U) &&
            (long_press_ms > debounce_ms) &&
            (very_long_press_ms > long_press_ms)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 检查按键逻辑 ID 是否有效
@para:key_id 按键逻辑 ID
@return: 1 表示有效，0 表示无效
*******************************************************************************/
static uint8_t key_id_is_valid(key_id_t key_id)
{
    return ((key_id < KEY_ID_MAX) &&
            (s_key_items != 0) &&
            ((uint8_t)key_id < s_key_item_count) &&
            (s_key_items[key_id].gpio_name != 0)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 将一个按键事件压入服务事件队列
@para:key_id 按键逻辑 ID
@para:press_type 按压类型
@return:
*******************************************************************************/
static void key_push_event(key_id_t key_id, key_press_type_t press_type)
{
    key_event_t event;

    if (s_key_event_queue == 0)
    {
        return;
    }

    event.key_id = key_id;
    event.press_type = press_type;

    if (event.press_type != 0U)
    {
        (void)xQueueSend(s_key_event_queue, &event, (TickType_t)0U);
    }
}

/*****************************************************************************
@brief: 读取指定按键当前是否处于按下状态
@para:key_id 按键逻辑 ID
@para:pressed 输出参数，1 表示按下，0 表示松开
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
static int key_read_pressed(key_id_t key_id, uint8_t *pressed)
{
    gpio_level_t level;
    int ret;

    if ((key_id_is_valid(key_id) == 0U) || (pressed == 0))
    {
        return RET_INVALID_PARAM;
    }

    ret = dev_gpio_read(s_key_items[key_id].gpio_name, &level);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    *pressed = (level == s_key_items[key_id].active_level) ? 1U : 0U;

    return RET_OK;
}

/*****************************************************************************
@brief: 扫描一个按键并更新消抖状态机
@para:key_id 按键逻辑 ID
@return:
*******************************************************************************/
static void key_scan_one(key_id_t key_id)
{
    key_runtime_t *runtime;
    TickType_t now_tick;
    uint32_t elapsed_ms;
    uint8_t pressed;

    if (key_read_pressed(key_id, &pressed) != RET_OK)
    {
        return;
    }

    runtime = &s_key_runtime[key_id];
    now_tick = xTaskGetTickCount();
    elapsed_ms = (uint32_t)((now_tick - runtime->press_tick) * portTICK_PERIOD_MS);

    switch (runtime->state)
    {
        case KEY_SCAN_RELEASE:
            /* 松开状态检测到有效按下后，记录按下起始时刻并进入消抖确认。 */
            if (pressed != 0U)
            {
                runtime->state = KEY_SCAN_CONFIRM;
                runtime->press_tick = now_tick;
            }
            break;

        case KEY_SCAN_CONFIRM:
            /* 消抖期间如果按键松开，认为是抖动；持续按下超过消抖时间才确认有效。 */
            if (pressed == 0U)
            {
                runtime->state = KEY_SCAN_RELEASE;
            }
            else if (elapsed_ms >= s_key_debounce_ms)
            {
                runtime->state = KEY_SCAN_SHORT_PRESS;
            }
            break;

        case KEY_SCAN_SHORT_PRESS:
            /* 有效按下后，如果在长按阈值前释放，上报短按事件。达到长按阈值时立即上报长按。 */
            if (pressed == 0U)
            {
                runtime->state = KEY_SCAN_RELEASE;
                key_push_event(key_id, KEY_PRESS_SHORT);
            }
            else if (elapsed_ms >= s_key_long_press_ms)
            {
                runtime->state = KEY_SCAN_LONG_PRESS;
                key_push_event(key_id, KEY_PRESS_LONG);
            }
            break;

        case KEY_SCAN_LONG_PRESS:
            /* 长按事件已触发，继续按住达到超长按阈值时立即上报超长按。 */
            if (pressed == 0U)
            {
                runtime->state = KEY_SCAN_RELEASE;
            }
            else if (elapsed_ms >= s_key_very_long_press_ms)
            {
                runtime->state = KEY_SCAN_VERY_LONG_PRESS;
                key_push_event(key_id, KEY_PRESS_VERY_LONG);
            }
            break;

        case KEY_SCAN_VERY_LONG_PRESS:
            /* 超长按事件已触发，释放时只复位状态，不再重复上报。 */
            if (pressed == 0U)
            {
                runtime->state = KEY_SCAN_RELEASE;
            }
            break;

        default:
            runtime->state = KEY_SCAN_RELEASE;
            runtime->press_tick = now_tick;
            break;
    }
}

/*****************************************************************************
@brief: 按键扫描软件定时器回调
@para:xTimer FreeRTOS 软件定时器句柄
@return:
*******************************************************************************/
static void key_timer_callback(TimerHandle_t xTimer)
{
    uint8_t i;

    (void)xTimer;

    for (i = 0U; i < s_key_item_count; i++)
    {
        key_scan_one((key_id_t)i);
    }
}

/*****************************************************************************
@brief: 初始化按键服务
@para:cfg 产品按键绑定和扫描时序配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
int key_service_init(const key_service_cfg_t *cfg)
{
    uint8_t i;

    if ((cfg == 0) || (cfg->items == 0) || (cfg->item_count == 0U) ||
        (cfg->item_count > (uint8_t)KEY_ID_MAX) || (cfg->scan_period_ms == 0U) ||
        (key_timing_is_valid(cfg->debounce_ms, cfg->long_press_ms, cfg->very_long_press_ms) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_key_items = cfg->items;
    s_key_item_count = cfg->item_count;
    s_key_debounce_ms = cfg->debounce_ms;
    s_key_long_press_ms = cfg->long_press_ms;
    s_key_very_long_press_ms = cfg->very_long_press_ms;

    for (i = 0U; i < (uint8_t)KEY_ID_MAX; i++)
    {
        s_key_runtime[i].state = KEY_SCAN_RELEASE;
        s_key_runtime[i].press_tick = 0U;
    }

    for (i = 0U; i < s_key_item_count; i++)
    {
        if (s_key_items[i].gpio_name == 0)
        {
            /* 配置表中的 GPIO 名称不能为空，否则无法通过 device 层访问硬件。 */
            return RET_INVALID_PARAM;
        }
    }

    /* 事件队列用于缓存按键扫描结果，上层通过 key_service_get_event/code 读取。 */
    s_key_event_queue = xQueueCreate(KEY_SERVICE_EVENT_QUEUE_LEN, sizeof(key_event_t));
    if (s_key_event_queue == 0)
    {
        return RET_NO_RESOURCE;
    }

    /* 使用 FreeRTOS 软件定时器周期扫描按键，避免在应用任务中分散消抖逻辑。 */
    s_key_timer_handle = xTimerCreate("KeyScan", pdMS_TO_TICKS(cfg->scan_period_ms), pdTRUE, (void *)0, key_timer_callback);
    if (s_key_timer_handle == 0)
    {
        return RET_NO_RESOURCE;
    }

    if (xTimerStart(s_key_timer_handle, 0U) != pdPASS)
    {
        return RET_FAIL;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 更新按键按压时序阈值
@para:debounce_ms 消抖确认时间，单位 ms
@para:long_press_ms 长按阈值，单位 ms
@para:very_long_press_ms 超长按阈值，单位 ms
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int key_service_set_timing(uint32_t debounce_ms, uint32_t long_press_ms, uint32_t very_long_press_ms)
{
    if (key_timing_is_valid(debounce_ms, long_press_ms, very_long_press_ms) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    s_key_debounce_ms = debounce_ms;
    s_key_long_press_ms = long_press_ms;
    s_key_very_long_press_ms = very_long_press_ms;

    return RET_OK;
}

/*****************************************************************************
@brief: 获取一个待处理的按键事件
@para:event 输出参数，用于保存按键事件
@return: RET_OK 表示成功获取事件，
         RET_NOT_FOUND 表示当前没有待处理事件，
         其他返回值表示失败
*******************************************************************************/
int key_service_get_event(key_event_t *event, uint32_t timeout_ms)
{
    if (event == 0)
    {
        return RET_INVALID_PARAM;
    }

    if (s_key_event_queue == 0)
    {
        return RET_NOT_INITED;
    }

    if (xQueueReceive(s_key_event_queue, event, timeout_ms) != pdTRUE)
    {
        return RET_NOT_FOUND;
    }

    return RET_OK;
}
