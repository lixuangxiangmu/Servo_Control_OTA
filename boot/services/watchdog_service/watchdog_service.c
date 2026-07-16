#include "watchdog_service.h"

#include "return_code.h"

static uint32_t s_alive_flags;
static uint32_t s_required_alive_mask;
static watchdog_feed_fn_t s_feed;

/*****************************************************************************
@brief: Initialize the watchdog policy service with default policy
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int watchdog_service_init(void)
{
    s_alive_flags = 0U;
    s_required_alive_mask = WDG_ALIVE_ALL;
    s_feed = 0;

    return RET_OK;
}

/*****************************************************************************
@brief: Configure required alive modules and hardware feed callback
@para:cfg Watchdog policy configuration
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int watchdog_service_config(const watchdog_service_cfg_t *cfg)
{
    if ((cfg == 0) || (cfg->required_alive_mask == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_required_alive_mask = cfg->required_alive_mask;
    s_feed = cfg->feed;
    s_alive_flags = 0U;

    return RET_OK;
}

/*****************************************************************************
@brief: Mark one or more modules as alive in the current feed window
@para:module_mask Bit mask composed of watchdog_alive_t values
@return:
*******************************************************************************/
void watchdog_service_mark_alive(uint32_t module_mask)
{
    s_alive_flags |= (module_mask & s_required_alive_mask);
}

/*****************************************************************************
@brief: Feed hardware watchdog only when all required modules are alive
@para:
@return:
*******************************************************************************/
void watchdog_service_poll(void)
{
    if ((s_alive_flags & s_required_alive_mask) == s_required_alive_mask)
    {
        if (s_feed != 0)
        {
            s_feed();
        }

        s_alive_flags = 0U;
    }
}

/*****************************************************************************
@brief: Get current alive flags for diagnostics
@para:
@return: Current alive mask
*******************************************************************************/
uint32_t watchdog_service_get_alive_flags(void)
{
    return s_alive_flags;
}
