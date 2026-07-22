#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "ota_image.h"

#include <stdint.h>

/*****************************************************************************
 * @brief Run the bootloader-side OTA serial protocol service.
 *
 * This function enters the OTA service main loop, which runs indefinitely
 * on UART3. It continuously reads incoming OTA protocol frames, parses them,
 * and dispatches recognized commands to their handlers. Responses are built
 * and transmitted back over the same UART.
 *
 * Behavior:
 * - If @p initial_info is provided and valid, it is used as the starting
 *   OTA state. This is typically the case when resuming an interrupted
 *   transfer — the EEPROM state is loaded by the caller and passed in.
 * - If @p initial_info is NULL or invalid, the service loads the EEPROM
 *   state internally and initializes defaults if no valid state exists.
 * - The function flushes the UART receive buffer on entry to discard any
 *   stale data, then enters an infinite read-parse-dispatch loop.
 * - This function NEVER returns. The only way to exit the OTA service is
 *   via a system reset (triggered by CMD_REBOOT or external reset).
 *
 * Threading Note:
 * - This is a bare-metal (no RTOS) bootloader implementation. The function
 *   runs on the main stack and blocks on UART reads. There are no
 *   interrupts or concurrent tasks — the protocol is purely polled.
 *
 * @param initial_info       Pointer to an OTA EEPROM info structure loaded
 *                           by the caller, or NULL to load internally.
 * @param initial_info_valid Non-zero if @p initial_info contains valid data
 *                           that should be used; zero to ignore and load
 *                           from EEPROM.
 */
void ota_service_task(const ota_eeprom_info_t *initial_info, uint8_t initial_info_valid);

#endif /* OTA_SERVICE_H */
