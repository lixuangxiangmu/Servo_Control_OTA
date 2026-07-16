#ifndef WATCHDOG_SERVICE_H
#define WATCHDOG_SERVICE_H

#include <stdint.h>

typedef enum
{
    WDG_ALIVE_MAIN  = (1UL << 0),
    WDG_ALIVE_COMM  = (1UL << 1),
    WDG_ALIVE_MOTOR = (1UL << 2),
    WDG_ALIVE_PARAM = (1UL << 3),
} watchdog_alive_t;

#define WDG_ALIVE_ALL (WDG_ALIVE_MAIN | WDG_ALIVE_COMM | WDG_ALIVE_MOTOR | WDG_ALIVE_PARAM)

typedef void (*watchdog_feed_fn_t)(void);

typedef struct
{
    uint32_t required_alive_mask;
    watchdog_feed_fn_t feed;
} watchdog_service_cfg_t;

/*****************************************************************************
@brief: Initialize the watchdog policy service with default policy
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int watchdog_service_init(void);

/*****************************************************************************
@brief: Configure required alive modules and hardware feed callback
@para:cfg Watchdog policy configuration
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int watchdog_service_config(const watchdog_service_cfg_t *cfg);

/*****************************************************************************
@brief: Mark one or more modules as alive in the current feed window
@para:module_mask Bit mask composed of watchdog_alive_t values
@return:
*******************************************************************************/
void watchdog_service_mark_alive(uint32_t module_mask);

/*****************************************************************************
@brief: Feed hardware watchdog only when all required modules are alive
@para:
@return:
*******************************************************************************/
void watchdog_service_poll(void);

/*****************************************************************************
@brief: Get current alive flags for diagnostics
@para:
@return: Current alive mask
*******************************************************************************/
uint32_t watchdog_service_get_alive_flags(void);

#endif
