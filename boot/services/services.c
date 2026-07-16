#include "services.h"

#include "fault_service/fault_service.h"
#include "return_code.h"
#include "watchdog_service/watchdog_service.h"

/*****************************************************************************
@brief: Initialize all business service modules
@para:
@return: RET_OK indicates successful initialization,
         other return values indicate failure
*******************************************************************************/
int services_init(void)
{
    int ret;

    ret = fault_service_init();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    ret = watchdog_service_init();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return RET_OK;
}
