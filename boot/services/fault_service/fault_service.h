#ifndef FAULT_SERVICE_H
#define FAULT_SERVICE_H

#include <stdint.h>

typedef enum
{
    FAULT_LEVEL_INFO = 0,
    FAULT_LEVEL_WARN,
    FAULT_LEVEL_ERROR,
    FAULT_LEVEL_FATAL,
} fault_level_t;

typedef enum
{
    FAULT_NONE = 0,
    FAULT_PARAM_CRC_ERROR,
    FAULT_FLASH_WRITE_FAIL,
    FAULT_MOTOR_TIMEOUT,
    FAULT_MOTOR_STALL,
    FAULT_UART_COMM_TIMEOUT,
    FAULT_OTA_VERIFY_FAIL,
    FAULT_WATCHDOG_RESET,
    FAULT_STACK_OVERFLOW,
    FAULT_MALLOC_FAILED,
    FAULT_CODE_MAX,
} fault_code_t;

typedef struct
{
    fault_code_t code;
    fault_level_t level;
    uint32_t active_mask;
    uint32_t report_count;
} fault_status_t;

/*****************************************************************************
@brief: Initialize the unified fault service
@para:
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_init(void);

/*****************************************************************************
@brief: Report a fault and mark it active
@para:code Fault code
@para:level Fault level
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_report(fault_code_t code, fault_level_t level);

/*****************************************************************************
@brief: Clear a specific active fault
@para:code Fault code
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_clear(fault_code_t code);

/*****************************************************************************
@brief: Query whether a specific fault is active
@para:code Fault code
@return: 1 means active, 0 means inactive or invalid
*******************************************************************************/
uint8_t fault_service_is_active(fault_code_t code);

/*****************************************************************************
@brief: Get the most recently reported fault code
@para:
@return: Last fault code
*******************************************************************************/
fault_code_t fault_service_get_last_fault(void);

/*****************************************************************************
@brief: Copy current fault status for log, communication, or diagnostics
@para:status Output status buffer
@return: RET_OK indicates success, other return values indicate failure
*******************************************************************************/
int fault_service_get_status(fault_status_t *status);

#endif
