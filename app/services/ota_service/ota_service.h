#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdint.h>

/** Return non-zero when a UART message begins with the binary OTA SOF. */
uint8_t ota_service_is_protocol_frame(const uint8_t *buf, uint32_t len);

/**
 * Parse and handle one App-side OTA request.
 *
 * Supported requests are CMD_GET_INFO and CMD_ENTER_OTA. A successful
 * CMD_ENTER_OTA response is transmitted before the device resets into Boot.
 */
int ota_service_process_frame(const uint8_t *buf, uint32_t len);

/** Legacy local entry point retained for existing callers. */
void app_ota_enter(void);

#endif /* OTA_SERVICE_H */
