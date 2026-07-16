#include "led_service.h"

#include "return_code.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#define LED_SERVICE_TIMER_PERIOD_MS 1U     //LED定时器 1ms

/* LED blink control state. */
typedef struct
{
    uint8_t active;          /* Whether blinking is enabled. */
    uint16_t interval_ms;    /* Toggle interval in milliseconds. */
    uint16_t elapsed_ms;     /* Accumulated time in milliseconds. */
    uint16_t toggle_count;   /* Remaining toggle count, 0 means continuous blinking. */
} led_blink_t;

/* LED hardware configuration injected by the application layer. */
static const led_service_item_cfg_t *s_led_items;
static uint8_t s_led_item_count;

/* LED blink runtime state table. */
static led_blink_t s_led_blink[LED_ID_MAX];
static TimerHandle_t g_led_timer_handle;


static uint8_t led_blink_has_active_item(void)
{
    for (uint8_t i = 0U; i < (uint8_t)LED_ID_MAX; i++)
    {
        if (s_led_blink[i].active != 0U)
        {
            return 1U;
        }
    }

    return 0U;
}


static void led_blink_stop_item(led_id_t led_id)
{
    s_led_blink[led_id].active = 0U;
    s_led_blink[led_id].interval_ms = 0U;
    s_led_blink[led_id].elapsed_ms = 0U;
    s_led_blink[led_id].toggle_count = 0U;
}


static void led_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    led_service_poll(LED_SERVICE_TIMER_PERIOD_MS);
}


/*****************************************************************************
@brief: Check whether the LED ID is valid
@para:led_id LED logical ID
@return: 1 indicates valid, 0 indicates invalid
*******************************************************************************/
static int led_id_is_valid(led_id_t led_id)
{
    return ((led_id < LED_ID_MAX) && (s_led_items != 0) && ((uint8_t)led_id < s_led_item_count) &&
            (s_led_items[led_id].gpio_name != 0)) ? 1 : 0;
}

/*****************************************************************************
@brief: Get the GPIO level when the LED is off
@para:led_id LED logical ID
@return: GPIO level to output when the LED is off
*******************************************************************************/
static gpio_level_t led_inactive_level(led_id_t led_id)
{
    return (s_led_items[led_id].active_level == GPIO_LEVEL_HIGH) ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
}

/*****************************************************************************
@brief: Initialize the LED service
@para:
@return: RET_OK indicates successful initialization,
         other return values indicate failure
*******************************************************************************/
int led_service_init(const led_service_cfg_t *cfg)
{
    if ((cfg == 0) || (cfg->items == 0) || (cfg->item_count == 0U) || (cfg->item_count > (uint8_t)LED_ID_MAX))
    {
        return RET_INVALID_PARAM;
    }

    s_led_items = cfg->items;
    s_led_item_count = cfg->item_count;

    for (uint8_t i = 0U; i < (uint8_t)LED_ID_MAX; i++)
    {
        s_led_blink[i].active = 0U;
        s_led_blink[i].interval_ms = 0U;
        s_led_blink[i].elapsed_ms = 0U;
        s_led_blink[i].toggle_count = 0U;
    }

    for (uint8_t i = 0U; i < s_led_item_count; i++)
    {
        (void)led_service_off((led_id_t)i);
    }

    g_led_timer_handle = xTimerCreate("LED Timer",
                                      pdMS_TO_TICKS(LED_SERVICE_TIMER_PERIOD_MS),
                                      pdTRUE,
                                      (void *)0,
                                      led_timer_callback);
    if(g_led_timer_handle == NULL)
    {
        return RET_NO_RESOURCE;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: Turn on the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_on(led_id_t led_id)
{
    if (led_id_is_valid(led_id) == 0)
    {
        return RET_INVALID_PARAM;
    }

    led_blink_stop_item(led_id);

    return dev_gpio_write(s_led_items[led_id].gpio_name, s_led_items[led_id].active_level);
}

/*****************************************************************************
@brief: Turn off the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_off(led_id_t led_id)
{
    if (led_id_is_valid(led_id) == 0)
    {
        return RET_INVALID_PARAM;
    }

    led_blink_stop_item(led_id);

    return dev_gpio_write(s_led_items[led_id].gpio_name, led_inactive_level(led_id));
}

/*****************************************************************************
@brief: Toggle the state of the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_toggle(led_id_t led_id)
{
    if (led_id_is_valid(led_id) == 0)
    {
        return RET_INVALID_PARAM;
    }

    return dev_gpio_toggle(s_led_items[led_id].gpio_name);
}

/*****************************************************************************
@brief: Set the state of the specified LED
@para:led_id LED logical ID
@para:state Target LED state
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_set_state(led_id_t led_id, led_state_t state)
{
    if (state == LED_STATE_ON)
    {
        return led_service_on(led_id);
    }

    return led_service_off(led_id);
}

/*****************************************************************************
@brief: Start blinking the specified LED
@para:led_id LED logical ID
@para:interval_ms LED toggle interval in milliseconds
@para:toggle_count LED toggle count, 0 means continuous blinking
@return: RET_OK indicates start success, other return values indicate failure
*******************************************************************************/
int led_service_blink_start(led_id_t led_id, uint16_t interval_ms, uint16_t toggle_count)
{
    if ((led_id_is_valid(led_id) == 0) || (interval_ms == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_led_blink[led_id].active = 1U;
    s_led_blink[led_id].interval_ms = interval_ms;
    s_led_blink[led_id].elapsed_ms = 0U;
    s_led_blink[led_id].toggle_count = toggle_count;

    if (g_led_timer_handle == NULL)
    {
        return RET_INVALID_STATE;
    }

    if (xTimerStart(g_led_timer_handle, 0U) != pdPASS)
    {
        return RET_FAIL;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: Poll and process LED blink states
@para:elapsed_ms Elapsed time since the last invocation in milliseconds
@return:
*******************************************************************************/
void led_service_poll(uint16_t elapsed_ms)
{
    for (uint8_t i = 0U; i < (uint8_t)LED_ID_MAX; i++)
    {
        if (s_led_blink[i].active == 0U)
        {
            continue;
        }

        s_led_blink[i].elapsed_ms = (uint16_t)(s_led_blink[i].elapsed_ms + elapsed_ms);
        if (s_led_blink[i].elapsed_ms < s_led_blink[i].interval_ms)
        {
            continue;
        }

        s_led_blink[i].elapsed_ms = 0U;
        led_service_toggle((led_id_t)i);

        if (s_led_blink[i].toggle_count > 0U)
        {
            s_led_blink[i].toggle_count--;
            if (s_led_blink[i].toggle_count == 0U)
            {
                led_blink_stop_item((led_id_t)i);
                led_service_off((led_id_t)i);
            }
        }
    }

    if ((g_led_timer_handle != NULL) && (led_blink_has_active_item() == 0U))
    {
        (void)xTimerStop(g_led_timer_handle, 0U);
    }
}
