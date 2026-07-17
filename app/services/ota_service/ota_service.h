/*****************************************************************************
 * @file    ota_service.h
 * @brief   App-side OTA frame discrimination and request dispatching
 *
 * This module sits between the Bluetooth UART receive path and the OTA
 * protocol layer. Its responsibilities are:
 *
 *   1. Frame discrimination — determine whether an incoming byte sequence
 *      is an OTA protocol frame (as opposed to normal runtime serial data).
 *   2. Protocol frame parsing and command dispatch — parse the frame,
 *      validate its structure and CRCs via ota_protocol_parse_frame(), then
 *      route recognized commands to their respective handlers.
 *   3. Bootloader entry — provide a legacy API (app_ota_enter) that writes
 *      the OTA-request state to persistent storage and resets the MCU into
 *      the Boot image.
 *
 * In normal operation the application main loop passes every UART payload
 * through ota_service_is_protocol_frame() first. Only frames that start with
 * the OTA SOF magic (0x55 0xAA) are forwarded to ota_service_process_frame()
 * for full processing; all other data is treated as the regular serial
 * command stream.
 *****************************************************************************/

#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <stdint.h>

/**
 * Test whether a received byte sequence begins with the OTA start-of-frame
 * delimiter bytes (SOF0 = 0x55, SOF1 = 0xAA).
 *
 * This is a lightweight, constant-time check that can be applied to every
 * UART packet without the overhead of full frame parsing. It is the primary
 * entry point for triaging incoming data between the OTA protocol path and
 * the normal runtime command path.
 *
 * @param buf  Pointer to the received data buffer (must not be NULL).
 * @param len  Number of bytes available in buf (must be >= 2 for a positive
 *             match; shorter buffers always return 0).
 * @return     1 if the buffer starts with a valid SOF sequence, 0 otherwise.
 */
uint8_t ota_service_is_protocol_frame(const uint8_t *buf, uint32_t len);

/**
 * Parse and handle one App-side OTA request frame.
 *
 * This function performs the following sequence:
 *   1. Parse the raw buffer into an ota_frame_t via ota_protocol_parse_frame().
 *   2. Verify the frame type is FRAME_TYPE_REQ (response frames are rejected).
 *   3. Dispatch to the appropriate handler based on the CMD field:
 *      - CMD_GET_INFO  — builds and transmits device identity, firmware
 *        versions, flash geometry, and feature capability bits.
 *      - CMD_ENTER_OTA — validates the supplied image metadata (product ID,
 *        hardware ID, target version, image size, CRC), persists the OTA
 *        session parameters to EEPROM, transmits the enter-accept response,
 *        and triggers an MCU reset into the Boot image.
 *      - Any other command — transmits an OTA_ERR_UNKNOWN_CMD status response.
 *
 * @param buf  Pointer to a complete raw OTA frame (must start with valid SOF).
 * @param len  Total byte-length of the frame buffer.
 * @return     RET_OK on successful processing, or a negative error code from
 *             the protocol layer or the Bluetooth transport.
 */
int ota_service_process_frame(const uint8_t *buf, uint32_t len);

/**
 * Legacy entry point for entering the OTA bootloader from application code.
 *
 * This function performs a local (non-network-triggered) bootloader entry:
 * it initializes the OTA session state in EEPROM, marks the state as
 * OTA_STATE_REQUEST, and issues an NVIC system reset. The Boot image will
 * detect the pending OTA state on startup and transition to the OTA transfer
 * phase.
 *
 * The function has no effect if the EEPROM save operation fails — in that
 * case the reset is skipped and the application continues running.
 *
 * @note This is retained for backwards compatibility with existing callers
 *       (e.g., a debug-menu command or a long-press gesture). New code should
 *       prefer the CMD_ENTER_OTA protocol path which includes full metadata
 *       validation before committing to the reset.
 */
void app_ota_enter(void);

#endif /* OTA_SERVICE_H */
