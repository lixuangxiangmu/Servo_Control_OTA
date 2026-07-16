/*****************************************************************************
 * @file    app_storage.h
 * @brief   Parameter persistence storage module header.
 *          Responsible for EEPROM-based persistent storage and verification
 *          of system parameters. Key responsibilities:
 *
 *          1. Parameter range definitions
 *             - Centrally defines the minimum, maximum, and default values
 *               for all configurable parameters.
 *             - Serves as the single authoritative source for system-wide
 *               parameter constraints.
 *
 *          2. EEPROM read/write encapsulation
 *             - Serializes the parameter structure to EEPROM with CRC16
 *               integrity protection.
 *             - On power-up, reads from EEPROM, deserializes, and
 *               automatically verifies both CRC and field ranges.
 *
 *          3. Fault-tolerant fallback
 *             - On EEPROM read failure / CRC error / parameter out-of-range,
 *               automatically falls back to factory default parameters.
 *             - After fallback, automatically writes the default parameters
 *               back to EEPROM to repair the corrupted storage region.
 *             - Tracks the parameter source via app_storage_source_t for
 *               power-on diagnostics.
 *
 *          4. Default parameter definitions
 *             - Provides factory default parameter values (initial angle,
 *               fold angle, power-off return behavior, etc.).
 *             - Follows the legacy product behavior for backward compatibility.
 * @author  LXA
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#ifndef APP_STORAGE_H
#define APP_STORAGE_H

#include "app_state.h"            /* Parameter structure and storage-source enumeration definitions */

#include <stdint.h>

/*===========================================================================
 * EEPROM Device Identifier
 *===========================================================================*/

/** EEPROM device name used for addressing in eeprom_read / eeprom_update calls */
#define APP_STORAGE_EEPROM_NAME             "eeprom0"

/*===========================================================================
 * Parameter Range Definitions — Servo Speed
 *===========================================================================*/

/** Servo speed minimum level (slowest) */
#define APP_PARAM_SERVO_SPEED_MIN           0U
/** Servo speed maximum level (fastest) */
#define APP_PARAM_SERVO_SPEED_MAX           5U
/** Servo speed factory default (fastest, prioritizing user experience) */
#define APP_PARAM_SERVO_SPEED_DEFAULT       5U

/*===========================================================================
 * Parameter Range Definitions — Initial Angle
 *===========================================================================*/

/** Initial angle minimum value (degrees) */
#define APP_PARAM_ANGLE_INIT_MIN            0U
/** Initial angle maximum value (degrees) */
#define APP_PARAM_ANGLE_INIT_MAX            60U
/** Initial angle factory default (degrees), follows legacy product behavior */
#define APP_PARAM_ANGLE_INIT_DEFAULT        0U

/*===========================================================================
 * Parameter Range Definitions — Fold Angle
 *===========================================================================*/

/** Fold angle minimum value (degrees) */
#define APP_PARAM_ANGLE_FOLD_MIN            15U
/** Fold angle maximum value (degrees) */
#define APP_PARAM_ANGLE_FOLD_MAX            179U
/** Fold angle factory default (degrees), follows legacy product behavior */
#define APP_PARAM_ANGLE_FOLD_DEFAULT        125U

/*===========================================================================
 * 参数值域定义 —— 堵转后的初始角度和折叠角度
 *===========================================================================*/
#define APP_LOCK_INIT_ANGLE                 10U
#define APP_LOCK_FOLD_ANGLE                 20U

/** Minimum long-press duration to enter lock mode (seconds) */
#define APP_PARAM_LOCK_SEC_MIN              1U
/** Maximum long-press duration to enter lock mode (seconds) */
#define APP_PARAM_LOCK_SEC_MAX              5U
/** Factory default long-press duration to enter lock mode (seconds) */
#define APP_PARAM_LOCK_SEC_DEFAULT          3U          /** 长按进入锁定出厂默认时间（秒） */

/** Minimum extra-long-press duration to exit lock mode (seconds) */
#define APP_PARAM_UNLOCK_SEC_MIN            5U
/** Maximum extra-long-press duration to exit lock mode (seconds) */
#define APP_PARAM_UNLOCK_SEC_MAX            20U
/** Factory default extra-long-press duration to exit lock mode (seconds) */
#define APP_PARAM_UNLOCK_SEC_DEFAULT        8U

/*===========================================================================
 * Public API
 *===========================================================================*/

/*****************************************************************************
 * @brief   Retrieve the factory default parameter set.
 *
 *          Populates params with factory default values:
 *            - Servo speed:        APP_PARAM_SERVO_SPEED_DEFAULT (fastest)
 *            - Fold angle:         APP_PARAM_ANGLE_FOLD_DEFAULT
 *            - Initial angle:      APP_PARAM_ANGLE_INIT_DEFAULT
 *            - Lock long-press:    APP_PARAM_LOCK_SEC_DEFAULT seconds
 *            - Unlock long-press:  APP_PARAM_UNLOCK_SEC_DEFAULT seconds
 *            - Power-off return:   enabled
 *            - Memory function:    disabled
 *            - Unlock mode:        ALL
 *            - Remote key code:    0 (unpaired)
 *
 * @param   params  Pointer to the destination parameter structure (caller-
 *                  allocated memory).
 *
 * @return  None (silently returns if params is NULL).
 *****************************************************************************/
void app_storage_get_default_params(app_servo_params_t *params);

/*****************************************************************************
 * @brief   Validate that all parameter fields are within their allowed ranges.
 *
 *          Each field in params is checked individually. Range validation is
 *          centralized here so that all code paths saving parameters pass
 *          through the same set of rules. Validation includes:
 *            - servo_speed:            0 ~ APP_PARAM_SERVO_SPEED_MAX
 *            - angle_fold:             APP_PARAM_ANGLE_FOLD_MIN ~ APP_PARAM_ANGLE_FOLD_MAX
 *            - angle_init:             0 ~ APP_PARAM_ANGLE_INIT_MAX
 *            - enter_lock_sec:         APP_PARAM_LOCK_SEC_MIN ~ APP_PARAM_LOCK_SEC_MAX
 *            - enter_unlock_sec:       APP_PARAM_UNLOCK_SEC_MIN ~ APP_PARAM_UNLOCK_SEC_MAX
 *            - power_off_return:       0 or 1
 *            - power_off_fold_state:   0 or 1
 *            - memory_function_enable: 0 or 1
 *            - unlock_mode:            0 or 1
 *
 * @param   params  Pointer to the parameter structure to validate.
 *
 * @return  RET_OK (0) if all fields are valid, RET_INVALID_PARAM (non-zero)
 *          if any field is out of range or params is NULL.
 *****************************************************************************/
int app_storage_params_is_valid(const app_servo_params_t *params);

/*****************************************************************************
 * @brief   Load parameters from EEPROM with automatic fault recovery.
 *
 *          Processing flow:
 *            1. Read raw data from EEPROM at address EE_SERVO_DATA_ADDR.
 *            2. If the read fails (IO error) -> use default parameters and
 *               record the source as DEFAULT_IO.
 *            3. Compute CRC16 and compare against the stored CRC value.
 *            4. If CRC mismatch -> use default parameters and record the
 *               source as DEFAULT_CRC.
 *            5. Deserialize into the parameter structure and validate all
 *               field ranges via app_storage_params_is_valid.
 *            6. If any field is out of range -> use default parameters and
 *               record the source as DEFAULT_RANGE.
 *            7. If all checks pass -> record the source as EEPROM.
 *            8. At any fallback step, the default parameters are
 *               automatically written back to EEPROM to repair the
 *               corrupted region.
 *
 *          On power-up read failure, CRC error, or parameter out-of-range,
 *          defaults are used and written back to EEPROM.
 *
 * @param   params  Pointer to the destination parameter structure (caller-
 *                  allocated memory).
 * @param   source  Pointer to a storage-source tracking variable. May be
 *                  NULL; if NULL, the source is not recorded.
 *
 * @return  RET_OK            Load succeeded (whether from EEPROM or defaults).
 *          RET_INVALID_PARAM  params is NULL.
 *****************************************************************************/
int app_storage_load_params(app_servo_params_t *params, app_storage_source_t *source);

/*****************************************************************************
 * @brief   Save parameters to EEPROM with CRC integrity protection.
 *
 *          Processing flow:
 *            1. Validate parameter ranges via app_storage_params_is_valid.
 *            2. Serialize parameters into the EEPROM record format
 *               (st_eeprom_servo_data).
 *            3. Compute and write the CRC16 checksum.
 *            4. Write to EEPROM via eeprom_update.
 *
 *          Uses eeprom_update (not eeprom_write):
 *            eeprom_update internally reads the current value first and only
 *            writes bytes that have changed, reducing the number of EEPROM
 *            write cycles and extending the device lifetime.
 *
 * @param   params  Pointer to the parameter structure to save (const).
 *
 * @return  RET_OK             Save succeeded.
 *          RET_INVALID_PARAM   Parameter range validation failed.
 *          RET_IO_ERROR        EEPROM write length mismatch.
 *          Other               Error code returned by the EEPROM driver.
 *****************************************************************************/
int app_storage_save_params(const app_servo_params_t *params);

#endif /* APP_STORAGE_H */
