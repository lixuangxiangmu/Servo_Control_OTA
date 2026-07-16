/*****************************************************************************
 * @file    app_storage.c
 * @brief   Parameter persistence storage module implementation.
 *          Responsible for EEPROM-based persistent storage, integrity
 *          verification, and fault tolerance of system parameters.
 *          Key responsibilities:
 *
 *          1. CRC16 data integrity verification
 *             - Computes a CRC16 checksum over the parameter record on write
 *               and stores it alongside the data.
 *             - On read, recomputes the CRC16 and compares it against the
 *               stored value to detect data corruption.
 *             - CRC covers only the payload data area; the CRC field itself
 *               is excluded from the computation to avoid circular dependency.
 *
 *          2. Parameter serialization / deserialization
 *             - app_params_t (application-layer parameters) <-> st_eeprom_servo_data
 *               (EEPROM storage format).
 *             - Field names may differ between the two structures (e.g.,
 *               angle_fold <-> threshold_angle_fold); dedicated conversion
 *               functions encapsulate these naming differences.
 *             - Reserved fields are zeroed before writing to prevent
 *               uninitialized bytes from affecting the CRC.
 *
 *          3. Power-on load with fault tolerance
 *             - Three-tier fallback chain: IO error -> CRC error -> range violation.
 *             - Failure at any tier causes a fallback to factory default parameters,
 *               which are then automatically written back to EEPROM to repair the
 *               corrupted region.
 *             - The parameter source is tracked via app_storage_source_t for
 *               power-on diagnostics.
 *
 *          4. EEPROM lifetime optimization
 *             - Uses eeprom_update rather than eeprom_write: the former internally
 *               performs a read-then-compare cycle and only writes changed bytes.
 *             - This reduces unnecessary erase/write cycles, extending the
 *               operational life of the EEPROM device.
 *
 *          5. Default parameter strategy
 *             - Factory defaults follow the legacy product behavior:
 *               30° initial angle, 130° fold angle, power-off return enabled.
 * @author  LXA
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#include "app_storage.h"

/* Low-level hardware and utility interfaces */
#include "app_data_store.h"       /* EEPROM data structure definition (st_eeprom_servo_data) */
#include "app_mem_map.h"          /* EEPROM address map (EE_SERVO_DATA_ADDR) */
#include "eeprom.h"               /* EEPROM read/write driver (eeprom_read / eeprom_update) */
#include "return_code.h"          /* Unified return codes */
#include "utils_lib.h"            /* CRC16 computation utility (utils_calculate_crc16) */

#include <stddef.h>               /* offsetof macro */
#include <stdint.h>
#include <string.h>               /* memset */

/*===========================================================================
 * Internal Utility Functions — CRC Verification
 *===========================================================================*/

/*****************************************************************************
 * @brief   Compute the CRC16 checksum of an EEPROM record.
 *
 *          The CRC covers only the payload data area; the crc16 field
 *          itself is excluded from the computation. The offsetof macro
 *          determines the byte length of the payload area, ensuring that
 *          future changes to the CRC field layout do not affect the
 *          verification logic.
 *
 * @param   record  Pointer to an EEPROM data record (const, read-only).
 *
 * @return  Computed CRC16 value.
 *****************************************************************************/
static uint16_t app_storage_calc_record_crc(const st_eeprom_servo_data *record)
{
    return utils_calculate_crc16((const uint8_t *)record, (uint16_t)offsetof(st_eeprom_servo_data, crc16));
}

/*****************************************************************************
 * @brief   Read the stored CRC16 value from an EEPROM record.
 *
 *          The CRC is stored in little-endian byte order in EEPROM
 *          (low byte first), which is convenient for direct inspection
 *          of the low byte during serial debugging.
 *
 * @param   record  Pointer to an EEPROM data record (const, read-only).
 *
 * @return  Stored CRC16 value (reassembled from two uint8_t fields in
 *          little-endian order).
 *****************************************************************************/
static uint16_t app_storage_get_record_crc(const st_eeprom_servo_data *record)
{
    return (uint16_t)record->crc16[0] | ((uint16_t)record->crc16[1] << 8);
}

/*****************************************************************************
 * @brief   Write a CRC16 value into an EEPROM record in little-endian order.
 *
 *          The high byte is written to crc16[1] and the low byte to
 *          crc16[0].
 *
 * @param   record  Pointer to an EEPROM data record (will be modified).
 * @param   crc     CRC16 value to write.
 *
 * @return  None.
 *****************************************************************************/
static void app_storage_set_record_crc(st_eeprom_servo_data *record, uint16_t crc)
{
    record->crc16[0] = (uint8_t)(crc & 0xFFU);
    record->crc16[1] = (uint8_t)((crc >> 8) & 0xFFU);
}

/*===========================================================================
 * Internal Utility Functions — Parameter Serialization / Deserialization
 *===========================================================================*/

/*****************************************************************************
 * @brief   Deserialize: EEPROM record -> application-layer parameters.
 *
 *          Maps each field from the raw EEPROM data structure to the
 *          application-layer parameter structure. The two structures
 *          may use different field names (for historical reasons); this
 *          function encapsulates those naming differences.
 *
 *          Field mapping:
 *            record->servo_speed              -> params->servo_speed
 *            record->threshold_angle_fold     -> params->angle_fold
 *            record->threshold_angle_init     -> params->angle_init
 *            record->enter_lock_sec           -> params->enter_lock_sec
 *            record->enter_unlock_sec         -> params->enter_unlock_sec
 *            record->power_off_return         -> params->power_off_return
 *            record->power_off_fold_state     -> params->power_off_fold_state
 *            record->memory_function_flag     -> params->memory_function_enable
 *            record->unlock_mode              -> params->unlock_mode
 *            record->remote_key_code[0..1]    -> params->remote_key_code
 *
 * @param   record  Pointer to an EEPROM data record (const, read-only).
 * @param   params  Pointer to the destination application-layer parameter
 *                  structure.
 *
 * @return  None.
 *****************************************************************************/
static void app_storage_record_to_params(const st_eeprom_servo_data *record, app_servo_params_t *params)
{
    params->servo_speed = record->servo_speed;
    params->angle_fold = record->threshold_angle_fold;
    params->angle_init = record->threshold_angle_init;
    params->enter_lock_sec = record->enter_lock_sec;
    params->enter_unlock_sec = record->enter_unlock_sec;
    params->power_off_return = record->power_off_return;
    params->power_off_fold_state = record->power_off_fold_state;
    params->memory_function_enable = record->memory_function_flag;
    params->unlock_mode = record->unlock_mode;
    params->remote_key_code = (uint16_t)record->remote_key_code[0] |
                              ((uint16_t)record->remote_key_code[1] << 8);
}

/*****************************************************************************
 * @brief   Serialize: application-layer parameters -> EEPROM record.
 *
 *          Maps each field from the application-layer parameter structure
 *          to the EEPROM data record. The record is zeroed before writing
 *          to ensure reserved fields and unused bytes have known values (0),
 *          preventing uninitialized data from affecting CRC consistency.
 *          The CRC16 checksum is automatically computed and written after
 *          serialization.
 *
 *          Field mapping is the inverse of app_storage_record_to_params.
 *
 * @param   params  Pointer to the application-layer parameter structure
 *                  (const, read-only).
 * @param   record  Pointer to the destination EEPROM data record (will be
 *                  fully overwritten).
 *
 * @return  None.
 *****************************************************************************/
static void app_storage_params_to_record(const app_servo_params_t *params, st_eeprom_servo_data *record)
{
    uint16_t temp_crc = 0;

    /* Zero the entire record first so that reserved fields and unused
     * bytes are set to a known value (0). This prevents uninitialized
     * data from affecting CRC computation consistency. */
    (void)memset(record, 0, sizeof(*record));

    /* Map each field individually */
    record->servo_speed = params->servo_speed;
    record->threshold_angle_fold = params->angle_fold;
    record->threshold_angle_init = params->angle_init;
    record->enter_lock_sec = params->enter_lock_sec;
    record->enter_unlock_sec = params->enter_unlock_sec;
    record->power_off_return = params->power_off_return;
    record->power_off_fold_state = params->power_off_fold_state;
    record->memory_function_flag = params->memory_function_enable;
    record->unlock_mode = params->unlock_mode;
    record->remote_key_code[0] = (uint8_t)(params->remote_key_code & 0xFFU);
    record->remote_key_code[1] = (uint8_t)((params->remote_key_code >> 8) & 0xFFU);

    /* Compute and write the CRC16 checksum */
    temp_crc = app_storage_calc_record_crc(record);
    app_storage_set_record_crc(record, temp_crc);
}

/*===========================================================================
 * Public API
 *===========================================================================*/

/*****************************************************************************
 * @brief   Retrieve the factory default parameter set.
 *
 *          Populates the given parameter structure with defaults that
 *          follow the legacy product behavior:
 *            - Initial angle: defined by APP_PARAM_ANGLE_INIT_DEFAULT
 *              (fits most mechanical configurations).
 *            - Fold angle: defined by APP_PARAM_ANGLE_FOLD_DEFAULT.
 *            - Power-off auto-return: enabled (safety-first).
 *            - Memory function: disabled by default (user must explicitly
 *              enable it).
 *            - Servo speed: highest setting (user-experience-first).
 *            - Remote key code: 0 (unpaired).
 *
 * @param   params  Pointer to the destination parameter structure (caller-
 *                  allocated memory).
 *
 * @return  None (silently returns if params is NULL).
 *****************************************************************************/
void app_storage_get_default_params(app_servo_params_t *params)
{
    if (params == 0)
    {
        return;
    }

    params->servo_speed = APP_PARAM_SERVO_SPEED_DEFAULT;
    params->angle_fold = APP_PARAM_ANGLE_FOLD_DEFAULT;
    params->angle_init = APP_PARAM_ANGLE_INIT_DEFAULT;
    params->enter_lock_sec = APP_PARAM_LOCK_SEC_DEFAULT;
    params->enter_unlock_sec = APP_PARAM_UNLOCK_SEC_DEFAULT;
    params->power_off_return = 0U;                          /* Power-off return: enabled */
    params->power_off_fold_state = (uint8_t)APP_SERVO_POSE_INIT; /* Last pose: initial position */
    params->memory_function_enable = 0U;                     /* Memory function: disabled */
    params->unlock_mode = (uint8_t)APP_UNLOCK_MODE_ALL;     /* Unlock mode: all channels */
    params->remote_key_code = 0U;                            /* Remote: unpaired */
}

/*****************************************************************************
 * @brief   Validate that all parameter fields are within their allowed ranges.
 *
 *          Each field is checked individually against its defined limits.
 *          All code paths that save parameters pass through this same set
 *          of rules before writing.
 *
 *          Validation items:
 *            servo_speed             0 ~ 5      Servo speed level
 *            angle_fold             MIN ~ MAX   Fold angle (degrees)
 *            angle_init             0 ~ MAX     Initial angle (degrees)
 *            enter_lock_sec         1 ~ 5       Lock long-press duration (seconds)
 *            enter_unlock_sec       5 ~ 20      Unlock extra-long-press duration (seconds)
 *            power_off_return       0 or 1      Power-off return strategy
 *            power_off_fold_state   0 or 1      Pre-power-loss pose
 *            memory_function_enable 0 or 1      Memory function on/off
 *            unlock_mode            0 or 1      Unlock mode selection
 *
 *          The range-check logic is kept consistent with the value-clamping
 *          logic in app_display_clamp_value. Together they form a dual-layer
 *          "clamp + validate" defense.
 *
 * @param   params  Pointer to the parameter structure to validate.
 *
 * @return  RET_OK (0) if all fields are valid, RET_INVALID_PARAM (non-zero)
 *          if any field is out of range or params is NULL.
 *****************************************************************************/
int app_storage_params_is_valid(const app_servo_params_t *params)
{
    if (params == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* --- Servo speed: 0 ~ APP_PARAM_SERVO_SPEED_MAX --- */
    if (params->servo_speed > APP_PARAM_SERVO_SPEED_MAX)
    {
        return RET_INVALID_PARAM;
    }

    /* --- Fold angle: APP_PARAM_ANGLE_FOLD_MIN ~ APP_PARAM_ANGLE_FOLD_MAX --- */
    if ((params->angle_fold < APP_PARAM_ANGLE_FOLD_MIN) || (params->angle_fold > APP_PARAM_ANGLE_FOLD_MAX))
    {
        return RET_INVALID_PARAM;
    }

    /* --- Initial angle: 0 ~ APP_PARAM_ANGLE_INIT_MAX --- */
    if (params->angle_init > APP_PARAM_ANGLE_INIT_MAX)
    {
        return RET_INVALID_PARAM;
    }

    /* --- Lock long-press duration: APP_PARAM_LOCK_SEC_MIN ~ APP_PARAM_LOCK_SEC_MAX seconds --- */
    if ((params->enter_lock_sec < APP_PARAM_LOCK_SEC_MIN) || (params->enter_lock_sec > APP_PARAM_LOCK_SEC_MAX))
    {
        return RET_INVALID_PARAM;
    }

    /* --- Unlock extra-long-press duration: APP_PARAM_UNLOCK_SEC_MIN ~ APP_PARAM_UNLOCK_SEC_MAX seconds --- */
    if ((params->enter_unlock_sec < APP_PARAM_UNLOCK_SEC_MIN) || (params->enter_unlock_sec > APP_PARAM_UNLOCK_SEC_MAX))
    {
        return RET_INVALID_PARAM;
    }

    /* --- Boolean-type fields: only 0 or 1 allowed --- */
    if (params->power_off_return > 1U)
    {
        return RET_INVALID_PARAM;
    }

    if (params->power_off_fold_state > (uint8_t)APP_SERVO_POSE_FOLD)
    {
        return RET_INVALID_PARAM;
    }

    if (params->memory_function_enable > 1U)
    {
        return RET_INVALID_PARAM;
    }

    if (params->unlock_mode > (uint8_t)APP_UNLOCK_MODE_BLE_ONLY)
    {
        return RET_INVALID_PARAM;
    }

    /* All validations passed */
    return RET_OK;
}

/*****************************************************************************
 * @brief   Load parameters from EEPROM with a three-tier fault-tolerance
 *          fallback chain.
 *
 *          Tier 1 — IO Error:
 *            If eeprom_read returns a byte count that does not match
 *            sizeof(record), an IO error is assumed (EEPROM chip not ready,
 *            bus fault, etc.). Falls back to default parameters with the
 *            source marked as DEFAULT_IO.
 *
 *          Tier 2 — CRC Checksum Error:
 *            If the EEPROM read succeeds but the CRC16 does not match,
 *            the stored data is considered corrupted (e.g., due to a
 *            power loss mid-write or electromagnetic interference).
 *            Falls back to default parameters with the source marked as
 *            DEFAULT_CRC.
 *
 *          Tier 3 — Parameter Range Violation:
 *            If the CRC is valid but one or more fields fall outside
 *            their legal range (e.g., after a firmware upgrade changed
 *            the valid range), falls back to default parameters with
 *            the source marked as DEFAULT_RANGE.
 *
 *          At every fallback step, app_storage_save_params is called
 *          automatically to write the default parameters back to EEPROM,
 *          repairing the corrupted storage region so that subsequent
 *          power-ups do not fall back again.
 *
 * @param   params  Pointer to the destination parameter structure (caller-
 *                  allocated memory).
 * @param   source  Pointer to a storage-source tracking variable. May be
 *                  NULL; if NULL, the source is not recorded.
 *
 * @return  RET_OK            Load succeeded (whether from EEPROM or defaults).
 *          RET_INVALID_PARAM  params is NULL.
 *****************************************************************************/
int app_storage_load_params(app_servo_params_t *params, app_storage_source_t *source)
{
    int ret;
    st_eeprom_servo_data record;
    uint16_t calc_crc;
    uint16_t store_crc;

    /* Null-pointer guard */
    if (params == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* Initialize source as unknown */
    if (source != 0)
    {
        *source = APP_STORAGE_SOURCE_UNKNOWN;
    }

    /* --- Tier 1: Attempt to read from EEPROM --- */
    ret = eeprom_read(APP_STORAGE_EEPROM_NAME, EE_SERVO_DATA_ADDR, (uint8_t *)&record, (uint32_t)sizeof(record));
    if (ret != (int)sizeof(record))
    {
        /* IO read failure: use default parameters and write them back to repair the EEPROM region. */
        app_storage_get_default_params(params);
        if (source != 0)
        {
            *source = APP_STORAGE_SOURCE_DEFAULT_IO;
        }
        (void)app_storage_save_params(params);
        return RET_OK;
    }

    /* --- Tier 2: CRC16 verification --- */
    calc_crc = app_storage_calc_record_crc(&record);    /* Recompute CRC over the EEPROM data */
    store_crc = app_storage_get_record_crc(&record);    /* Retrieve the stored CRC from EEPROM */
    if (calc_crc != store_crc)
    {
        /* CRC mismatch: data is corrupted. Use default parameters and
         * write them back to repair the EEPROM region. */
        app_storage_get_default_params(params);
        if (source != 0)
        {
            *source = APP_STORAGE_SOURCE_DEFAULT_CRC;
        }
        (void)app_storage_save_params(params);
        return RET_OK;
    }

    /* CRC passed: deserialize EEPROM record into application-layer parameters */
    app_storage_record_to_params(&record, params);

    /* --- Tier 3: Parameter range validation --- */
    if (app_storage_params_is_valid(params) != 0)
    {
        /* Range violation: possibly due to a firmware upgrade that changed
         * valid parameter ranges. Use default parameters and write them
         * back to repair the EEPROM region. */
        app_storage_get_default_params(params);
        if (source != 0)
        {
            *source = APP_STORAGE_SOURCE_DEFAULT_RANGE;
        }
        (void)app_storage_save_params(params);
        return RET_OK;
    }

    /* All checks passed: parameters originate from valid EEPROM data */
    if (source != 0)
    {
        *source = APP_STORAGE_SOURCE_EEPROM;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief   Save parameters to EEPROM with CRC integrity protection.
 *
 *          Processing flow:
 *            1. Validate parameter ranges; return RET_INVALID_PARAM if any
 *               field is out of bounds.
 *            2. Serialize: app_params_t -> st_eeprom_servo_data (CRC is
 *               computed automatically during serialization).
 *            3. Write to EEPROM via eeprom_update (which internally reads
 *               first and only writes changed bytes, reducing wear).
 *            4. Verify write length: if the returned byte count does not
 *               match the expected record size, return an error code.
 *
 *          eeprom_update vs. eeprom_write:
 *            eeprom_update internally reads the current EEPROM value and
 *            only performs writes on bytes that have actually changed.
 *            This significantly reduces the number of erase/write cycles,
 *            extending the EEPROM device lifetime. A typical EEPROM is
 *            rated for ~1 million erase/write cycles; since only a few
 *            bytes typically change per save, the update strategy can
 *            extend the effective lifetime by an order of magnitude.
 *
 * @param   params  Pointer to the parameter structure to save (const).
 *
 * @return  RET_OK             Save succeeded.
 *          RET_INVALID_PARAM   Parameter range validation failed.
 *          RET_IO_ERROR        EEPROM write length mismatch.
 *          Other               Error code returned by the EEPROM driver.
 *****************************************************************************/
int app_storage_save_params(const app_servo_params_t *params)
{
    st_eeprom_servo_data record;
    int ret;

    /* Validate parameters before saving */
    if (app_storage_params_is_valid(params) != 0)
    {
        return RET_INVALID_PARAM;
    }

    /* Serialize into EEPROM record format (CRC is computed automatically) */
    app_storage_params_to_record(params, &record);

    /* Write to EEPROM using update (not write) to minimize erase/wear cycles */
    ret = eeprom_update(APP_STORAGE_EEPROM_NAME, EE_SERVO_DATA_ADDR, (const uint8_t *)&record, (uint32_t)sizeof(record));
    if (ret != (int)sizeof(record))
    {
        /* Write length mismatch: eeprom_update may itself return a driver-
         * level error code. */
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        return RET_IO_ERROR;
    }

    return RET_OK;
}
