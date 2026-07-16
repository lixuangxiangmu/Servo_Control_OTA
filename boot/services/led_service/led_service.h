#ifndef LED_SERVICE_H
#define LED_SERVICE_H

#include "dev_gpio.h"

#include <stdint.h>

/* LED logical ID definitions. */
typedef enum
{
    LED_SERVO_STATUS = 0,     /* System status indicator LED. */
    LED_ID_MAX,         /* LED count boundary value, not used as an actual LED. */
} led_id_t;

/* LED business state definitions. */
typedef enum
{
    LED_STATE_OFF = 0,  /* Off. */
    LED_STATE_ON = 1,   /* On. */
} led_state_t;

/* Single LED binding supplied by the application configuration layer. */
typedef struct
{
    const char *gpio_name;
    gpio_level_t active_level;
} led_service_item_cfg_t;

/* LED service configuration supplied by application/app_config.c. */
typedef struct
{
    const led_service_item_cfg_t *items;
    uint8_t item_count;
} led_service_cfg_t;

/*****************************************************************************
@brief: Initialize the LED service
@para:cfg Product LED binding configuration
@return: RET_OK indicates successful initialization,
         other return values indicate failure
*******************************************************************************/
int led_service_init(const led_service_cfg_t *cfg);

/*****************************************************************************
@brief: Turn on the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_on(led_id_t led_id);

/*****************************************************************************
@brief: Turn off the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_off(led_id_t led_id);

/*****************************************************************************
@brief: Toggle the state of the specified LED
@para:led_id LED logical ID
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_toggle(led_id_t led_id);

/*****************************************************************************
@brief: Set the state of the specified LED
@para:led_id LED logical ID
@para:state Target LED state
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int led_service_set_state(led_id_t led_id, led_state_t state);

/*****************************************************************************
@brief: Start blinking the specified LED
@para:led_id LED logical ID
@para:interval_ms LED toggle interval in milliseconds
@para:toggle_count LED toggle count, 0 means continuous blinking
@return: RET_OK indicates start success, other return values indicate failure
*******************************************************************************/
int led_service_blink_start(led_id_t led_id, uint16_t interval_ms, uint16_t toggle_count);

/*****************************************************************************
@brief: Poll and process LED blink states
@para:elapsed_ms Elapsed time since the last invocation in milliseconds
@return:
*******************************************************************************/
void led_service_poll(uint16_t elapsed_ms);

#endif
