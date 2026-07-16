/*****************************************************************************
 * @file    app_hmi.h
 * @brief   HMI (human-machine interface) module header.
 *          Provides centralized management for the servo controller input
 *          channels, including keys, encoder, remote controller, and Bluetooth.
 *          Parses user commands and dispatches them to modules such as the
 *          control layer and display layer.
 *          Only the initialization API is exposed externally; each input
 *          channel runs internally as an independent FreeRTOS task.
 * @author  GD32 Servo Control Team
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#ifndef APP_HMI_H
#define APP_HMI_H

/*****************************************************************************
 * @brief:  Initialize the HMI module.
 *          Creates the mutex and four input-processing tasks:
 *            - AppKey      Key scan task
 *            - AppEncoder  Encoder scan task
 *            - AppRemote   Remote receiver task
 *            - AppBT       Bluetooth UART receive/transmit task
 *          The function is idempotent and safe to call repeatedly; reentry is
 *          prevented by the s_hmi_inited flag.
 * @para:   None
 * @return: RET_OK           Initialization succeeded, including repeated calls
 *                            after successful initialization
 *          RET_NO_RESOURCE   Mutex creation or any task creation failed
 *****************************************************************************/
int app_hmi_init(void);

#endif /* APP_HMI_H */
