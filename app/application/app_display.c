/*****************************************************************************
 * @file    app_display.c
 * @brief   Digital tube display module implementation
 *          Central HMI display hub of the system, responsible for the following:
 *
 *          1. Normal angle display
 *             - Real-time refresh of current servo angle (driven by FreeRTOS task
 *               at 20ms cycle)
 *             - Display format: first digit = mode code '0', last three digits =
 *               angle value (0 ~ 180)
 *
 *          2. Parameter editing interaction
 *             - Supports three parameter edit modes: fold angle, initial angle,
 *               speed level
 *             - Edit value blink indication (350ms on/off cycle), visually
 *               indicating edit state
 *             - Timeout auto-save: auto-commit edit value and exit edit mode
 *               after 5 seconds of inactivity
 *             - Manual exit: supports save-and-exit (save=1) or cancel-and-exit
 *               (save=0)
 *
 *          3. Value range safety management
 *             - Fold angle range: 60° ~ 179° (APP_PARAM_ANGLE_FOLD_MIN ~ MAX)
 *             - Initial angle range: 0° ~ 60° (APP_PARAM_ANGLE_INIT_MIN ~ MAX)
 *             - Speed level range: 0 ~ 5 (APP_PARAM_SERVO_SPEED_MIN ~ MAX)
 *             - Auto-clamp after each adjustment to prevent out-of-range writes
 *
 *          4. Thread safety
 *             - FreeRTOS Mutex protects the display runtime state struct
 *             - Concurrent access by display task and HMI input (button/encoder
 *               ISRs) is serialized via locking to guarantee data consistency
 *             - Mutex uses blocking wait (portMAX_DELAY) to ensure the critical
 *               section is always enterable
 *
 *          5. Digital tube driver
 *             - Drives the 4-digit digital tube via digital_tube_service interface
 *             - Supports independent per-digit configuration (incl. decimal point)
 *             - Auto-clears before display to avoid ghosting
 * @author  LXA
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#include "app_display.h"

/* Application layer module interfaces */
#include "app_state.h"            /* System global parameters & runtime state read/write */
#include "app_storage.h"          /* Parameter persistent storage (EEPROM) */
#include "digital_tube_service.h" /* 4-digit digital tube driver interface */
#include "return_code.h"          /* Unified return codes (RET_OK / RET_IS_OK / RET_IS_ERR etc.) */

/* FreeRTOS kernel components */
#include "FreeRTOS.h"
#include "semphr.h"               /* Mutex */
#include "task.h"                 /* Task creation & delay */

#include <stdint.h>

/*===========================================================================
 * Macro definitions — Task configuration
 *===========================================================================*/

/** Display refresh task stack depth (words) */
#define APP_DISPLAY_TASK_STACK              384U

/** Display refresh task priority (lower number = lower priority; display is the lowest-priority task) */
#define APP_DISPLAY_TASK_PRIO               1U

/** Display refresh period (ms). 20ms = 50Hz refresh rate, perceived as continuous by the human eye */
#define APP_DISPLAY_REFRESH_MS              20U

/** Edit mode timeout (ms). Auto-commit and exit edit mode after 5 seconds of inactivity */
#define APP_DISPLAY_TIMEOUT_MS              5000U

/** Edit value blink period (ms). 350ms on + 350ms off = 700ms full cycle */
#define APP_DISPLAY_BLINK_MS                350U

/*===========================================================================
 * Display runtime struct
 *===========================================================================*/

/**
 * @brief Display module runtime state
 *
 * Records the current display mode, edit value, blink state, and time bases.
 * Read periodically by the display task; modified by HMI input interfaces
 * (app_display_adjust, etc.).
 * Protected by mutex s_display_mutex for concurrent access.
 */
typedef struct
{
    app_display_mode_t mode;        /**< Current display / edit mode */
    uint8_t edit_value;             /**< Current parameter value being edited (meaningless in NORMAL mode) */
    uint8_t blink_visible;          /**< Blink visibility flag: 1 = show value this cycle, 0 = hide value this cycle */
    TickType_t activity_tick;       /**< System tick count of the last user operation (for timeout detection) */
    TickType_t blink_tick;          /**< System tick count of the last blink state toggle (for blink cycle control) */
} app_display_runtime_t;

/*===========================================================================
 * Module-level static variables
 *===========================================================================*/

/** Display state mutex handle (FreeRTOS Mutex), protects concurrent access to s_display struct */
static SemaphoreHandle_t s_display_mutex;

/** Display module runtime state (only accessed via locked interfaces) */
static app_display_runtime_t s_display_runtime_state;

/** Display module initialization flag: 0 = not initialized, 1 = initialized (provides idempotency) */
static uint8_t s_display_inited;

/*===========================================================================
 * Internal utility functions
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Timer check based on FreeRTOS tick count
 *          Uses delta comparison rather than absolute comparison, correctly
 *          handling TickType_t wraparound (overflow rollover).
 * @para:   now_tick    Current system tick count (from xTaskGetTickCount())
 * @para:   last_tick   Tick count at the start of the interval
 * @para:   interval_ms Desired timer interval in milliseconds
 * @return: 1U = elapsed, 0U = not elapsed
 *****************************************************************************/
static uint8_t app_display_tick_elapsed(TickType_t now_tick, TickType_t last_tick, uint32_t interval_ms)
{
    return (((uint32_t)(now_tick - last_tick) * portTICK_PERIOD_MS) >= interval_ms) ? 1U : 0U;
}

/*****************************************************************************
 * @brief:  Acquire the display state mutex (lock)
 *          Blocks until the mutex is successfully acquired. Caller must lock
 *          before accessing s_display.
 *          Uses portMAX_DELAY for indefinite wait to guarantee the critical
 *          section is always enterable.
 * @para:   None
 * @return: RET_OK          Lock acquired
 *          RET_NOT_INITED   Mutex not yet created (module not initialized)
 *          RET_TIMEOUT      Mutex acquisition timed out (should never occur
 *                           since portMAX_DELAY is used)
 *****************************************************************************/
static int app_display_lock(void)
{
    if (s_display_mutex == 0)
    {
        return RET_NOT_INITED;
    }

    if (xSemaphoreTake(s_display_mutex, portMAX_DELAY) != pdTRUE)
    {
        return RET_TIMEOUT;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Release the display state mutex (unlock)
 *          Paired with app_display_lock; must unlock after operating on s_display.
 * @para:   None
 * @return: None
 *****************************************************************************/
static void app_display_unlock(void)
{
    (void)xSemaphoreGive(s_display_mutex);
}

/*****************************************************************************
 * @brief:  Get the current value of the parameter corresponding to the edit mode
 *          Reads the parameter value for the current mode from the params struct
 *          to serve as the initial edit value.
 *          Mapping:
 *            SET_FOLD -> params.angle_fold (fold angle)
 *            SET_INIT -> params.angle_init (initial angle)
 *            SET_SPEED -> params.servo_speed (servo speed level)
 * @para:   params  Pointer to current parameter struct (const, read-only)
 * @para:   mode    Edit mode
 * @return: Corresponding parameter value; returns 0 in NORMAL mode
 *****************************************************************************/
static uint8_t app_display_param_value(const app_servo_params_t *params, app_display_mode_t mode)
{
    switch (mode)
    {
        case APP_DISPLAY_MODE_SET_FOLD:
            return params->angle_fold;

        case APP_DISPLAY_MODE_SET_INIT:
            return params->angle_init;

        case APP_DISPLAY_MODE_SET_SPEED:
            return params->servo_speed;

        default:
            return 0U;
    }
}

/*****************************************************************************
 * @brief:  Get the next edit mode (cyclic toggle)
 *          Mode transition sequence:
 *            NORMAL -> SET_FOLD -> SET_INIT -> SET_SPEED -> NORMAL
 *          Forms a closed loop so the user can cycle through all edit items.
 * @para:   mode  Current mode
 * @return: Next mode
 *****************************************************************************/
static app_display_mode_t app_display_next_mode(app_display_mode_t mode)
{
    switch (mode)
    {
        case APP_DISPLAY_MODE_NORMAL:
            return APP_DISPLAY_MODE_SET_FOLD;

        case APP_DISPLAY_MODE_SET_FOLD:
            return APP_DISPLAY_MODE_SET_INIT;

        case APP_DISPLAY_MODE_SET_INIT:
            return APP_DISPLAY_MODE_SET_SPEED;

        default:
            return APP_DISPLAY_MODE_NORMAL;
    }
}

/*****************************************************************************
 * @brief:  Commit edit value: write parameter and persist to EEPROM
 *          Writes edit value to param struct -> runtime state -> EEPROM
 *          persistent storage.
 *          Three-step atomic operation: any step failure returns immediately
 *          with an error code; subsequent steps are skipped.
 *          No-op in NORMAL mode (no parameter to commit).
 * @para:   mode   Edit mode (determines which param field to write)
 * @para:   value  Parameter value to commit
 * @return: RET_OK             Commit succeeded
 *          RET_INVALID_PARAM   Unknown edit mode
 *          Other               Corresponding error code from param read/write
 *                              or storage failure
 *****************************************************************************/
static int app_display_commit_value(app_display_mode_t mode, uint8_t value)
{
    app_servo_params_t params;
    int ret;

    /* NORMAL mode requires no commit */
    if (mode == APP_DISPLAY_MODE_NORMAL)
    {
        return RET_OK;
    }

    /* Read current parameters */
    ret = app_state_get_servo_params(&params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Update corresponding field based on edit mode */
    switch (mode)
    {
        case APP_DISPLAY_MODE_SET_FOLD:     // Fold angle
            params.angle_fold = value;
            break;

        case APP_DISPLAY_MODE_SET_INIT:     // Initial angle
            params.angle_init = value;
            break;

        case APP_DISPLAY_MODE_SET_SPEED:    // Speed
            params.servo_speed = value;
            break;

        default:
            return RET_INVALID_PARAM;
    }

    /* Write to runtime state */
    ret = app_state_set_servo_params(&params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Persist to EEPROM */
    ret = app_storage_save_params(&params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Apply edit value to runtime state in real time (memory only, no persist)
 *          Differs from app_display_commit_value: only updates app_state runtime
 *          parameters without writing to EEPROM. Used for real-time preview
 *          during editing to reduce EEPROM write cycles.
 * @para:   mode   Edit mode (determines which param field to write)
 * @para:   value  Parameter value to apply
 * @return: RET_OK             Apply succeeded
 *          RET_INVALID_PARAM   Unknown edit mode
 *          Other               Corresponding error code from param read/write
 *                              failure
 *****************************************************************************/
static int app_display_apply_value_to_state(app_display_mode_t mode, uint8_t value)
{
    app_servo_params_t params;
    int ret;

    /* NORMAL mode requires no apply */
    if (mode == APP_DISPLAY_MODE_NORMAL)
    {
        return RET_OK;
    }

    ret = app_state_get_servo_params(&params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    switch (mode)
    {
        case APP_DISPLAY_MODE_SET_FOLD:
            params.angle_fold = value;
            break;

        case APP_DISPLAY_MODE_SET_INIT:
            params.angle_init = value;
            break;

        case APP_DISPLAY_MODE_SET_SPEED:
            params.servo_speed = value;
            break;

        default:
            return RET_INVALID_PARAM;
    }

    return app_state_set_servo_params(&params);
}

/*****************************************************************************
 * @brief:  Clamp parameter value to valid range
 *          Clamps the input value to the legal range defined by the current
 *          edit mode.
 *          Value range definitions (from app_storage.h):
 *            - Fold angle:   60° ~ 179°
 *            - Initial angle: 0° ~  60°
 *            - Speed level:   0  ~   5
 *          Uses int16_t as intermediate type to safely handle overflow from
 *          uint8_t addition/subtraction.
 * @para:   mode   Edit mode (determines clamp range)
 * @para:   value  Value to clamp (int16_t, allows temporary out-of-range)
 * @return: Clamped uint8_t value; returns 0 in NORMAL mode
 *****************************************************************************/
static uint8_t app_display_clamp_value(app_display_mode_t mode, int16_t value)
{
    int16_t min_value = 0;
    int16_t max_value = 0;

    /* Set value range boundaries based on edit mode */
    switch (mode)
    {
        case APP_DISPLAY_MODE_SET_FOLD:
            min_value = APP_PARAM_ANGLE_FOLD_MIN;
            max_value = APP_PARAM_ANGLE_FOLD_MAX;
            break;

        case APP_DISPLAY_MODE_SET_INIT:
            min_value = APP_PARAM_ANGLE_INIT_MIN;
            max_value = APP_PARAM_ANGLE_INIT_MAX;
            break;

        case APP_DISPLAY_MODE_SET_SPEED:
            min_value = APP_PARAM_SERVO_SPEED_MIN;
            max_value = APP_PARAM_SERVO_SPEED_MAX;
            break;

        default:
            return 0U;
    }

    /* Clamp to [min, max] range */
    if (value < min_value)
    {
        value = min_value;
    }
    else if (value > max_value)
    {
        value = max_value;
    }

    return (uint8_t)value;
}

/*****************************************************************************
 * @brief:  Display mode code and numeric value on the digital tube
 *          Display format:
 *            - DIG0: mode code (0=NORMAL, 1=SET_FOLD, 2=SET_INIT, 3=SET_SPEED)
 *              with decimal point lit as a persistent mode indicator
 *            - DIG1~3: hundreds, tens, ones of the value (auto-suppress leading
 *              zeros)
 *            - When value_visible=0, only the mode code is shown; the numeric
 *              value is hidden (blink-off state)
 *
 *          Leading-zero suppression logic:
 *            - value >= 100: show all three digits
 *            - 10 <= value < 100: show tens and ones (hundreds digit off)
 *            - value < 10: show ones only (hundreds and tens digits off)
 *          This keeps the numeric display clean and free of leading-zero clutter.
 * @para:   mode          Display mode (enum value used directly as mode code)
 * @para:   value         Numeric value to display (0 ~ 999; auto-clamped to 999)
 * @para:   value_visible Numeric value visibility flag: 1 = show value,
 *                        0 = show mode code only (blink-off state)
 * @return: RET_OK        Display succeeded
 *          Other          Corresponding error code from digital tube driver
 *                         failure
 *****************************************************************************/
static int app_display_show_mode_value(app_display_mode_t mode, uint16_t value, uint8_t value_visible)
{
    int ret;

    /* Upper limit protection: max displayable value on last 3 digits is 999 */
    if (value > 999U)
    {
        value = 999U;
    }

    /* Clear display first to avoid ghosting */
    ret = digital_tube_service_clear();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* DIG0: mode code with decimal point lit as edit indicator */
    ret = digital_tube_service_set_digit(0U, (uint8_t)mode, 1U);
    if (RET_IS_ERR(ret) || (value_visible == 0U))
    {
        return ret;
    }

    /* DIG1: hundreds (hidden when value < 100 for leading-zero suppression) */
    if (value >= 100U)
    {
        ret = digital_tube_service_set_digit(1U, (uint8_t)(value / 100U), 0U);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    /* DIG2: tens (hidden when value < 10) */
    if (value >= 10U)
    {
        ret = digital_tube_service_set_digit(2U, (uint8_t)((value / 10U) % 10U), 0U);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    /* DIG3: ones (always displayed — at least one digit is always shown) */
    return digital_tube_service_set_digit(3U, (uint8_t)(value % 10U), 0U);
}

/*****************************************************************************
 * @brief:  Display refresh task (AppDisplay)
 *          Runs in a 20ms periodic loop, responsible for real-time digital
 *          tube refresh:
 *
 *          Per-cycle processing flow:
 *            1. Lock and read display runtime state (mode / edit_value /
 *               blink_visible)
 *            2. Check edit timeout: if not in NORMAL mode and 5 seconds have
 *               elapsed since last activity, auto-commit and exit edit mode
 *            3. Update blink state: toggle blink_visible every 350ms (on ↔ off)
 *            4. Handle timeout commit: commit triggered by timeout is executed
 *               outside the lock to avoid deadlock
 *            5. Refresh digital tube:
 *               - NORMAL mode: display current servo angle (read from app_state)
 *               - Edit mode: display mode code + edit value (visibility
 *                 controlled by blink_visible)
 *            6. Unlock and delay 20ms, then enter next cycle
 *
 *          Design notes:
 *            - Runs as an independent low-priority task so it does not interfere
 *              with high-real-time tasks such as motor control and sensor reading
 *            - Commit is executed after unlocking to avoid holding the lock
 *              during time-consuming EEPROM writes
 *            - Blink is implemented by alternating show/hide of the numeric
 *              value rather than toggling the tube power — the mode code
 *              remains always visible so the user never loses track of the
 *              current edit item
 * @para:   arg  Task parameter (unused, pass NULL)
 * @return: None (infinite loop task, never returns)
 *****************************************************************************/
static void app_display_task(void *arg)
{
    app_display_mode_t mode;
    app_display_mode_t commit_mode;
    app_servo_runtime_state_t servo_runtime;
    TickType_t now_tick;
    uint8_t value;
    uint8_t visible;
    uint8_t commit_value;

    (void)arg;

    while (1)
    {
        commit_mode = APP_DISPLAY_MODE_NORMAL;
        commit_value = 0U;
        now_tick = xTaskGetTickCount();

        /* --- Critical section: lock, read, and update display state --- */
        if (app_display_lock() == RET_OK)
        {
            /*
             * Timeout detection: if not in NORMAL mode and more than 5 seconds
             * have elapsed since last activity, auto-commit and exit.
             * commit_mode / commit_value are saved for execution outside the
             * lock to avoid holding the mutex during EEPROM writes.
             */
            if ((s_display_runtime_state.mode != APP_DISPLAY_MODE_NORMAL) &&
                (app_display_tick_elapsed(now_tick, s_display_runtime_state.activity_tick, APP_DISPLAY_TIMEOUT_MS) != 0U))
            {
                commit_mode = s_display_runtime_state.mode;         // Only assigned on timeout
                commit_value = s_display_runtime_state.edit_value;
                s_display_runtime_state.mode = APP_DISPLAY_MODE_NORMAL;
                (void)app_state_set_display_mode(APP_DISPLAY_MODE_NORMAL);
            }

            /* Blink cycle control: toggle visibility every 350ms */
            if ((s_display_runtime_state.mode != APP_DISPLAY_MODE_NORMAL) &&
                (app_display_tick_elapsed(now_tick, s_display_runtime_state.blink_tick, APP_DISPLAY_BLINK_MS) != 0U))
            {
                s_display_runtime_state.blink_visible = (s_display_runtime_state.blink_visible == 0U) ? 1U : 0U;
                s_display_runtime_state.blink_tick = now_tick;
            }

            /* Snapshot current state for use outside lock */
            mode = s_display_runtime_state.mode;
            value = s_display_runtime_state.edit_value;
            visible = s_display_runtime_state.blink_visible;
            app_display_unlock();

            /* --- Outside lock: execute timeout commit (involves EEPROM write — time-consuming) --- */
            if (commit_mode != APP_DISPLAY_MODE_NORMAL)
            {
                (void)app_display_commit_value(commit_mode, commit_value);
            }

            /* --- Refresh digital tube display --- */
            if (mode == APP_DISPLAY_MODE_NORMAL)
            {
                /* Normal mode: display current real-time servo angle */
                if (app_state_get_runtime(&servo_runtime) == RET_OK)
                {
                    (void)app_display_show_mode_value(APP_DISPLAY_MODE_NORMAL, servo_runtime.current_angle, 1U);
                }
            }
            else
            {
                /* Edit mode: display mode code + edit value (blink-controlled visibility) */
                (void)app_display_show_mode_value(mode, value, visible);
            }
        }

        /* 20ms refresh period (50Hz) for smooth, flicker-free display */
        vTaskDelay(pdMS_TO_TICKS(APP_DISPLAY_REFRESH_MS));
    }
}

/*===========================================================================
 * Public Interface
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Cycle to the next edit mode
 *          Flow:
 *            1. Read current parameters (to obtain the initial edit value for
 *               the new mode)
 *            2. Lock
 *            3. Record old mode and old value (for commit outside lock)
 *            4. Switch to next mode, initialize blink state and activity timestamp
 *            5. If entering an edit mode, load the corresponding parameter value
 *               as the initial edit value
 *            6. Sync new mode to app_state (so other modules can read the
 *               display mode)
 *            7. Unlock
 *            8. Commit the old mode's edit value outside the lock (if the old
 *               mode was an edit mode)
 *
 *          Design rationale:
 *            Committing the old value is done outside the lock to avoid
 *            blocking the display task refresh during EEPROM writes.
 *          Mode transition path: NORMAL -> SET_FOLD -> SET_INIT -> SET_SPEED -> NORMAL
 * @para:   None
 * @return: RET_OK          Switch succeeded
 *          RET_NOT_INITED   app_state not yet initialized
 *          Other            Corresponding error code from lock failure
 *****************************************************************************/
int app_display_cycle_edit_mode(void)
{
    app_display_mode_t old_mode;
    app_display_mode_t next_mode;
    uint8_t old_value;
    app_servo_params_t params;
    TickType_t now_tick;
    int ret;

    /* Read current parameters for use as edit initial value */
    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return RET_NOT_INITED;
    }

    ret = app_display_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Record old state (for commit outside lock) */
    old_mode = s_display_runtime_state.mode;
    old_value = s_display_runtime_state.edit_value;
    next_mode = app_display_next_mode(s_display_runtime_state.mode);
    now_tick = xTaskGetTickCount();

    /* Switch to new mode, reset blink and timeout timers */
    s_display_runtime_state.mode = next_mode;
    s_display_runtime_state.blink_visible = 1U;
    s_display_runtime_state.activity_tick = now_tick;
    s_display_runtime_state.blink_tick = now_tick;

    /* Load corresponding parameter value as edit initial value when entering edit mode */
    if (next_mode != APP_DISPLAY_MODE_NORMAL)
    {
        s_display_runtime_state.edit_value = app_display_param_value(&params, next_mode);
    }

    /* Sync to app_state */
    (void)app_state_set_display_mode(next_mode);
    app_display_unlock();

    /* Commit old value outside lock when previous mode was edit mode (avoid holding lock during EEPROM write) */
    if (old_mode != APP_DISPLAY_MODE_NORMAL)
    {
        (void)app_display_commit_value(old_mode, old_value);
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Adjust parameter value in the current edit mode
 *          Processing flow:
 *            1. Lock
 *            2. Validate mode: return RET_INVALID_STATE if in NORMAL mode
 *               (adjustment not allowed)
 *            3. Apply step delta to the current edit value
 *            4. Clamp to legal range via app_display_clamp_value
 *            5. Refresh activity timestamp and blink state (restart blink from
 *               visible state)
 *            6. Unlock
 *            7. Apply new value to app_state runtime parameters in real time
 *               (memory only, no EEPROM write)
 *
 *          Design rationale:
 *            During adjustment, only the in-memory parameters are updated to
 *            minimize Flash write cycles. The final save is performed by
 *            timeout auto-commit or manual exit via app_display_commit_value.
 * @para:   step  Adjustment step: positive = increase, negative = decrease
 *                (typically +1 or -1)
 * @return: RET_OK              Adjustment succeeded
 *          RET_INVALID_STATE    Currently in NORMAL mode, no parameter to adjust
 *          Other                Corresponding error code from lock or state
 *                              write failure
 *****************************************************************************/
int app_display_adjust(int8_t step)
{
    app_display_mode_t mode;
    uint8_t value;
    int ret;

    ret = app_display_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* No parameter to adjust in NORMAL mode — reject operation */
    if (s_display_runtime_state.mode == APP_DISPLAY_MODE_NORMAL)
    {
        app_display_unlock();
        return RET_INVALID_STATE;
    }

    /* Apply step and clamp to valid range */
    s_display_runtime_state.edit_value = app_display_clamp_value(s_display_runtime_state.mode, (int16_t)s_display_runtime_state.edit_value + step);
    s_display_runtime_state.activity_tick = xTaskGetTickCount();
    s_display_runtime_state.blink_visible = 1U;     // Keep visible while encoder is rotating
    s_display_runtime_state.blink_tick = s_display_runtime_state.activity_tick;
    mode = s_display_runtime_state.mode;
    value = s_display_runtime_state.edit_value;
    app_display_unlock();

    /* Apply to runtime state immediately (memory only) for instant feedback */
    return app_display_apply_value_to_state(mode, value);
}

/*****************************************************************************
 * @brief:  Exit edit mode
 *          Processing flow:
 *            1. Lock
 *            2. Record old mode and old value
 *            3. Restore display mode to NORMAL
 *            4. Sync to app_state
 *            5. Unlock
 *            6. If save=1 and old mode was an edit mode, commit edit value
 *               to EEPROM
 *
 *          Two exit modes:
 *            - save=1: Confirm and save — persist edit value to EEPROM
 *            - save=0: Cancel — discard the modified value (parameter retains
 *              the value from before editing)
 * @para:   save  1U = save edit value to EEPROM, 0U = discard edit value
 * @return: RET_OK          Exit succeeded
 *          Other            Corresponding error code from lock or storage
 *                           failure
 *****************************************************************************/
int app_display_exit_edit(uint8_t save)
{
    app_display_mode_t old_mode;
    uint8_t old_value;
    int ret;

    ret = app_display_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Record old state, restore to NORMAL mode */
    old_mode = s_display_runtime_state.mode;
    old_value = s_display_runtime_state.edit_value;
    s_display_runtime_state.mode = APP_DISPLAY_MODE_NORMAL;
    (void)app_state_set_display_mode(APP_DISPLAY_MODE_NORMAL);
    app_display_unlock();

    /* Only commit when save requested and previous mode was indeed an edit mode */
    if ((save != 0U) && (old_mode != APP_DISPLAY_MODE_NORMAL))
    {
        return app_display_commit_value(old_mode, old_value);
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Display module initialization
 *          Completes the following in order:
 *            1. Idempotency check (s_display_inited flag, prevents re-entry)
 *            2. Create mutex (protects s_display runtime state)
 *            3. Initialize runtime state:
 *               - Default NORMAL mode (display servo angle on power-up)
 *               - Blink visible flag set to 1
 *               - Activity and blink timestamps initialized to current tick
 *            4. Create display refresh task AppDisplay (stack 384W, priority 1)
 *            5. Set s_display_inited = 1
 *
 *          Notes:
 *            - Mutex creation failure returns RET_NO_RESOURCE
 *            - Task creation failure returns RET_NO_RESOURCE (does not roll
 *              back the already-created mutex)
 *            - The display task runs at the system's lowest priority so it
 *              does not interfere with high-real-time tasks (control, sensing)
 * @para:   None
 * @return: RET_OK           Initialization succeeded (including redundant calls
 *                           after first init)
 *          RET_NO_RESOURCE   Mutex or task creation failed (insufficient memory)
 *****************************************************************************/
int app_display_init(void)
{
    /* Idempotency guarantee: return immediately if already initialized */
    if (s_display_inited != 0U)
    {
        return RET_OK;
    }

    /* Create mutex to protect display runtime state */
    s_display_mutex = xSemaphoreCreateMutex();
    if (s_display_mutex == NULL)
    {
        return RET_NO_RESOURCE;
    }

    /* Initialize display runtime state: default normal angle display mode */
    s_display_runtime_state.mode = APP_DISPLAY_MODE_NORMAL;
    s_display_runtime_state.edit_value = 0U;
    s_display_runtime_state.blink_visible = 1U;
    s_display_runtime_state.activity_tick = xTaskGetTickCount();
    s_display_runtime_state.blink_tick = s_display_runtime_state.activity_tick;

    /* Create display refresh task (lowest priority — does not interfere with real-time control) */
    if (xTaskCreate(app_display_task, "AppDisplay", APP_DISPLAY_TASK_STACK, 0, APP_DISPLAY_TASK_PRIO, 0) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Mark initialization complete */
    s_display_inited = 1U;

    return RET_OK;
}
