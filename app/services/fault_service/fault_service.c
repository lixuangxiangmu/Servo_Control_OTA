#include "fault_service.h"

#include "return_code.h"

static uint32_t s_active_faults;
static fault_code_t s_last_fault;
static fault_level_t s_last_level;
static uint32_t s_report_count;

static uint8_t fault_code_is_valid(fault_code_t code)
{
    return ((code > FAULT_NONE) && (code < FAULT_CODE_MAX)) ? 1U : 0U;
}

static uint8_t fault_level_is_valid(fault_level_t level)
{
    return (level <= FAULT_LEVEL_FATAL) ? 1U : 0U;
}

/*****************************************************************************
@brief: Initialize the unified fault service
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_init(void)
{
    s_active_faults = 0U;
    s_last_fault = FAULT_NONE;
    s_last_level = FAULT_LEVEL_INFO;
    s_report_count = 0U;

    return RET_OK;
}

/*****************************************************************************
@brief: Report a fault and mark it active
@para:code Fault code
@para:level Fault level
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_report(fault_code_t code, fault_level_t level)
{
    if ((fault_code_is_valid(code) == 0U) || (fault_level_is_valid(level) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    s_active_faults |= (1UL << (uint32_t)code);
    s_last_fault = code;
    s_last_level = level;
    s_report_count++;

    return RET_OK;
}

/*****************************************************************************
@brief: Clear a specific active fault
@para:code Fault code
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_clear(fault_code_t code)
{
    if (fault_code_is_valid(code) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    s_active_faults &= ~(1UL << (uint32_t)code);

    if ((s_active_faults == 0U) && (s_last_fault == code))
    {
        s_last_fault = FAULT_NONE;
        s_last_level = FAULT_LEVEL_INFO;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: Query whether a specific fault is active
@para:code Fault code
@return: 1 means active, 0 means inactive or invalid
*******************************************************************************/
uint8_t fault_service_is_active(fault_code_t code)
{
    if (fault_code_is_valid(code) == 0U)
    {
        return 0U;
    }

    return ((s_active_faults & (1UL << (uint32_t)code)) != 0U) ? 1U : 0U;
}

/*****************************************************************************
@brief: Get the most recently reported fault code
@para:
@return: Last fault code
*******************************************************************************/
fault_code_t fault_service_get_last_fault(void)
{
    return s_last_fault;
}

/*****************************************************************************
@brief: Copy current fault status for log, communication, or diagnostics
@para:status Output status buffer
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_get_status(fault_status_t *status)
{
    if (status == 0)
    {
        return RET_INVALID_PARAM;
    }

    status->code = s_last_fault;
    status->level = s_last_level;
    status->active_mask = s_active_faults;
    status->report_count = s_report_count;

    return RET_OK;
}
