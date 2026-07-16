/*
 * FreeRTOS Kernel V11.1.0
 * Copyright (C) 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Include standard types and the system clock variable only in C compiler
   environments. */
#if defined(__ICCARM__) || defined(__CC_ARM) || defined(__GNUC__)
  #include <stddef.h>
  #include <stdint.h>
  extern uint32_t SystemCoreClock;
#endif

/******************************************************************************/
/* Hardware description related configuration. */
/******************************************************************************/

/* CPU clock frequency used by the FreeRTOS kernel. */
#define configCPU_CLOCK_HZ                         ( ( unsigned long ) SystemCoreClock )

/******************************************************************************/
/* Scheduling behavior related configuration. */
/******************************************************************************/

/* System tick frequency in Hz. */
#define configTICK_RATE_HZ                         ( 1000U )

/* Enable preemptive scheduling. */
#define configUSE_PREEMPTION                       1

/* Enable time slicing for tasks of equal priority. */
#define configUSE_TIME_SLICING                     1

/* Do not use port-optimized task selection algorithm. */
#define configUSE_PORT_OPTIMISED_TASK_SELECTION    0

/* Do not enable tickless low-power idle mode. */
#define configUSE_TICKLESS_IDLE                    0

/* Maximum number of task priorities supported by the system. */
#define configMAX_PRIORITIES                       16U

/* Minimum task stack depth. */
#define configMINIMAL_STACK_SIZE                   256U

/* Maximum task name length. */
#define configMAX_TASK_NAME_LEN                    30U

/* Tick count type width. */
#define configTICK_TYPE_WIDTH_IN_BITS              TICK_TYPE_WIDTH_32_BITS

/* The idle task may yield the CPU to tasks of equal priority. */
#define configIDLE_SHOULD_YIELD                    1

/* Number of entries in the notification array per task. */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES      1U

/* Queue registry size. 0 means the queue registry is not used. */
#define configQUEUE_REGISTRY_SIZE                  0U

/* Enable FreeRTOS backward compatibility interface. */
#define configENABLE_BACKWARD_COMPATIBILITY        1

/* Number of task local storage pointers. */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS    0

/* Stack depth type. */
#define configSTACK_DEPTH_TYPE                     size_t

/* Message buffer length type. */
#define configMESSAGE_BUFFER_LENGTH_TYPE           size_t

/* Do not enable newlib reentrant support. */
#define configUSE_NEWLIB_REENTRANT                 0

/******************************************************************************/
/* Software timer related configuration. */
/******************************************************************************/

/* Enable FreeRTOS software timers. */
#define configUSE_TIMERS                           1

/* Software timer task priority. */
#define configTIMER_TASK_PRIORITY                  ( configMAX_PRIORITIES - 1U )

/* Software timer task stack depth. */
#define configTIMER_TASK_STACK_DEPTH               ( configMINIMAL_STACK_SIZE * 2 )

/* Software timer command queue length. */
#define configTIMER_QUEUE_LENGTH                   10U

/******************************************************************************/
/* Memory allocation related configuration. */
/******************************************************************************/

/* Support static memory allocation. */
#define configSUPPORT_STATIC_ALLOCATION            1

/* Support dynamic memory allocation. */
#define configSUPPORT_DYNAMIC_ALLOCATION           1

/* Total size of the FreeRTOS dynamic heap. */
#define configTOTAL_HEAP_SIZE                      ( ( size_t ) 24 * 1024U )

/* The application layer does not define a custom FreeRTOS heap array. */
#define configAPPLICATION_ALLOCATED_HEAP           0

/* Task stacks are not allocated from a separate heap. */
#define configSTACK_ALLOCATION_FROM_SEPARATE_HEAP  0

/* Do not use the reduced-size ListItem structure. */
#define configUSE_MINI_LIST_ITEM                   0

/******************************************************************************/
/* Interrupt nesting behavior configuration. */
/******************************************************************************/

/* Number of hardware priority bits implemented by GD32F10x Cortex-M3 NVIC. */
#ifdef __NVIC_PRIO_BITS
  #define configPRIO_BITS                          __NVIC_PRIO_BITS
#else
  #define configPRIO_BITS                          4U
#endif

/* Lowest interrupt priority value used by the FreeRTOS kernel. */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY    15U

/* Highest NVIC priority that may call FreeRTOS FromISR APIs. */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5U

/* Kernel interrupt priority. */
#define configKERNEL_INTERRUPT_PRIORITY            ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8U - configPRIO_BITS ) )

/* Highest interrupt priority from which FreeRTOS API calls can be made. */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY       ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8U - configPRIO_BITS ) )

/* API call interrupt priority compatibility configuration. */
#define configMAX_API_CALL_INTERRUPT_PRIORITY      configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY

/******************************************************************************/
/* Hook function related configuration. */
/******************************************************************************/

/* Do not enable the idle task hook. */
#define configUSE_IDLE_HOOK                        0

/* Do not enable the tick hook. */
#define configUSE_TICK_HOOK                        0

/* Do not enable the memory allocation failure hook. */
#define configUSE_MALLOC_FAILED_HOOK               0

/* Do not enable the timer daemon task startup hook. */
#define configUSE_DAEMON_TASK_STARTUP_HOOK         0

/* Do not enable stack overflow detection. */
#define configCHECK_FOR_STACK_OVERFLOW             0

/******************************************************************************/
/* Runtime statistics and tracing related configuration. */
/******************************************************************************/

/* Do not enable runtime statistics. */
#define configGENERATE_RUN_TIME_STATS              0

/* Do not enable the trace debugging facility. */
#define configUSE_TRACE_FACILITY                   0

/* Do not enable statistics formatting functions. */
#define configUSE_STATS_FORMATTING_FUNCTIONS       0

/* Use the kernel-provided static memory interface. */
#define configKERNEL_PROVIDED_STATIC_MEMORY        1

/******************************************************************************/
/* FreeRTOS feature trimming configuration. */
/******************************************************************************/

/* Enable task notifications. */
#define configUSE_TASK_NOTIFICATIONS               1

/* Enable mutexes. */
#define configUSE_MUTEXES                          1

/* Enable recursive mutexes. */
#define configUSE_RECURSIVE_MUTEXES                1

/* Enable counting semaphores. */
#define configUSE_COUNTING_SEMAPHORES              1

/* Enable queue sets. */
#define configUSE_QUEUE_SETS                       1

/* Enable application task tags. */
#define configUSE_APPLICATION_TASK_TAG             1

/* Include the set task priority API. */
#define INCLUDE_vTaskPrioritySet                   1

/* Include the get task priority API. */
#define INCLUDE_uxTaskPriorityGet                  1

/* Include the delete task API. */
#define INCLUDE_vTaskDelete                        1

/* Include the suspend task API. */
#define INCLUDE_vTaskSuspend                       1

/* Include the resume task from ISR API. */
#define INCLUDE_xResumeFromISR                     1

/* Include the delay until API. */
#define INCLUDE_vTaskDelayUntil                    1

/* Include the standard delay API. */
#define INCLUDE_vTaskDelay                         1

/* Include the get scheduler state API. */
#define INCLUDE_xTaskGetSchedulerState             1

/* Include the get current task handle API. */
#define INCLUDE_xTaskGetCurrentTaskHandle          1

/* Include the get task stack high water mark API. */
#define INCLUDE_uxTaskGetStackHighWaterMark        1

/* Include the get idle task handle API. */
#define INCLUDE_xTaskGetIdleTaskHandle             1

/* Include the get task state API. */
#define INCLUDE_eTaskGetState                      1

/* Include the set event group bits from ISR API. */
#define INCLUDE_xEventGroupSetBitFromISR           1

/* Include the timer pend function call API. */
#define INCLUDE_xTimerPendFunctionCall             1

/* Include the abort task delay API. */
#define INCLUDE_xTaskAbortDelay                    1

/* Include the get task handle by name API. */
#define INCLUDE_xTaskGetHandle                     1

/* Include the resume task from ISR API. */
#define INCLUDE_xTaskResumeFromISR                 1

/* Map the FreeRTOS SVC interrupt handler to the CMSIS name. */
#define vPortSVCHandler                            SVC_Handler

/* Map the FreeRTOS SysTick interrupt handler to the CMSIS name. */
#define xPortSysTickHandler                        SysTick_Handler

/* Map the FreeRTOS PendSV interrupt handler to the CMSIS name. */
#define xPortPendSVHandler                         PendSV_Handler

#endif /* FREERTOS_CONFIG_H */
