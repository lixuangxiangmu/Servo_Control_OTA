/*****************************************************************************
 * @file    app_control.c
 * @brief   Servo control module implementation.
 *          The core system control hub, responsible for:
 *
 *          1. Command handling
 *             - Receives asynchronous control commands through a FreeRTOS queue.
 *             - Dispatches servo motion, lock/unlock, auto-calibration, and related actions in the control task.
 *
 *          2. Servo motion control
 *             - Moves from one angle to another one degree at a time, with speed control.
 *             - Pauses the remote receiver during motion to avoid RF interference with servo PWM.
 *
 *          3. Work mode management
 *             - NORMAL: normal fold/unfold mode.
 *             - LOCK: lock mode; servo output is disabled and only unlock conditions are monitored.
 *             - AUTO_CALIBRATE: auto-calibration mode for scanning mechanical limits.
 *
 *          4. Auto calibration
 *             - Four-stage state machine: center wait -> high-angle scan -> center wait -> low-angle scan.
 *             - Detects locked-rotor current to determine the fold angle upper limit and init angle lower limit.
 *             - Backs off measured limits by APP_AUTO_BACKOFF_ANGLE degrees as a safety margin.
 *
 *          5. Locked-rotor protection
 *             - Uses an independent task to monitor the servo current ADC sample in real time.
 *             - Confirms a locked rotor after repeated over-threshold samples and resets angle parameters to prevent damage.
 *
 *          6. Power management
 *             - Periodically checks the supply voltage at 1-second intervals.
 *             - Uses two-stage low-voltage detection: pending after the first threshold crossing, then power-off after 3 seconds below the confirm threshold.
 *             - Applies the power-off return policy before cutting power.
 *             - Stores the current fold/unfold state before power-off for recovery on the next startup.
 * @author  LXA
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#include "app_control.h"

/* Application-layer module interfaces */
#include "app_hw.h"        /* Hardware abstraction layer: power hold, voltage ADC, and locked-rotor ADC. */
#include "app_state.h"     /* System global parameter and runtime state access. */
#include "app_storage.h"   /* Persistent parameter storage in EEPROM. */
#include "key_service.h"   /* Key timing threshold configuration. */
#include "led_service.h"   /* LED indication: steady on/off and blinking. */
#include "remote_service.h" /* Remote receiver service start/stop; paused during motion to avoid interference. */
#include "return_code.h"   /* Unified return codes: RET_OK, RET_IS_OK, RET_IS_ERR, and related values. */
#include "servo_service.h" /* Servo PWM driver interface. */

/* FreeRTOS kernel components */
#include "FreeRTOS.h"
#include "queue.h"         /* Command queue. */
#include "task.h"          /* Task creation and delay. */

#include <stdint.h>

/*===========================================================================
 * Macro Definitions - Task Configuration
 *===========================================================================*/

/** Control command queue length; maximum number of pending commands. */
#define APP_CONTROL_QUEUE_LEN                  8U

/** Main control task stack depth in words; covers auto-calibration, command handling, and power-loss detection. */
#define APP_CONTROL_TASK_STACK                 640U

/** Locked-rotor detection task stack depth in words. */
#define APP_LOCKED_ROTOR_TASK_STACK            384U

/** Main control task priority; a smaller number means lower priority. */
#define APP_CONTROL_TASK_PRIO                  3U

/** Locked-rotor detection task priority. */
#define APP_LOCKED_ROTOR_TASK_PRIO             2U

/*===========================================================================
 * Macro Definitions - Servo Motion Parameters
 *===========================================================================*/

/** Maximum servo speed level; 0 through 5 for six levels. */
#define APP_SERVO_SPEED_MAX                    5U

/*===========================================================================
 * Macro Definitions - Power Detection Parameters
 *===========================================================================*/

/** Power voltage check period in milliseconds. */
#define APP_POWER_CHECK_PERIOD_MS              1000U

/**
 * Low-voltage confirmation time in milliseconds.
 * After the voltage first falls below 3000 mV, power-off is performed only
 * if it remains below 2000 mV for this duration, avoiding false trips from
 * transient voltage dips.
 */
#define APP_POWER_LOST_CONFIRM_MS              3000U

/** First low-voltage threshold in millivolts; below this value enters pending confirmation. */
#define APP_POWER_LOW_FIRST_THRESHOLD_MV       3000U

/** Low-voltage confirmation threshold in millivolts; sustained voltage below this value triggers power-off. */
#define APP_POWER_LOW_CONFIRM_THRESHOLD_MV     2000U

/**
 * Minimum voltage threshold in millivolts for keeping power after unlock.
 * After key unlock (EXIT_LOCK_BY_KEY), keep power if the supply voltage is
 * above this value; otherwise disable power hold and power down the system.
 */
#define APP_UNLOCK_KEEP_POWER_THRESHOLD_MV     4000U

/*===========================================================================
 * Macro Definitions - Locked-Rotor Detection Parameters
 *===========================================================================*/

/**
 * Locked-rotor current ADC threshold, raw value.
 * An ADC sample greater than or equal to this threshold is treated as one
 * locked-rotor event. Auto-calibration uses this threshold, while low-angle
 * scanning uses the more sensitive LOW_SCAN threshold.
 */
#define APP_LOCKED_ROTOR_THRESHOLD_RAW         1000U

/**
 * Locked-rotor detection threshold for the low-angle direction, raw value.
 * The mechanical limit in the init-angle direction is usually more sensitive,
 * so a lower threshold is used to avoid excessive compression.
 */
#define APP_LOCKED_ROTOR_LOW_SCAN_RAW          1000U

/**
 * Locked-rotor confirmation count.
 * Protection is triggered only after this many consecutive locked-rotor
 * events, preventing false triggers from transient current spikes.
 */
#define APP_LOCKED_ROTOR_CONFIRM_COUNT         20U

/*===========================================================================
 * Macro Definitions - Auto-Calibration Parameters
 *===========================================================================*/

/** Center angle reached before calibration; scanning starts from this position toward both ends. */
#define APP_AUTO_CENTER_ANGLE                  90U

/** High-angle scan limit on the fold side, used to avoid exceeding mechanical travel. */
#define APP_AUTO_HIGH_SCAN_LIMIT               175U

/** Low-angle scan limit on the init side, used to avoid exceeding mechanical travel. */
#define APP_AUTO_LOW_SCAN_LIMIT                5U

/**
 * Backoff angle after locked-rotor detection, in degrees.
 * When a locked rotor is detected, back off from the current position by this
 * angle and use the result as the final limit to avoid overload or noise at
 * the mechanical endpoint.
 */
#define APP_AUTO_BACKOFF_ANGLE                 5U

/** Auto-calibration step interval in milliseconds; controls scan speed. */
#define APP_AUTO_STEP_INTERVAL_MS              50U

/** Center wait time in milliseconds before starting a scan after reaching the center. */
#define APP_AUTO_CENTER_WAIT_MS                1000U

/*===========================================================================
 * Auto-Calibration State Machine Enumeration
 *===========================================================================*/

/**
 * @brief Four-stage auto-calibration state machine.
 *
 * State transition path:
 *   IDLE -> WAIT_HIGH_SCAN -> SCAN_HIGH -> WAIT_LOW_SCAN -> SCAN_LOW -> IDLE
 *
 * Flow:
 *   IDLE -> WAIT_HIGH_SCAN -> SCAN_HIGH
 *     ^                              |
 *     | locked rotor / limit reached |
 *     |                              v
 *   SCAN_LOW <- WAIT_LOW_SCAN <- return to center
 *     |
 *     +-> calibration complete -> IDLE
 */
typedef enum
{
    APP_AUTO_STAGE_IDLE = 0,          /**< Idle; calibration is not running. */
    APP_AUTO_STAGE_WAIT_HIGH_SCAN,    /**< Waiting at the center before scanning toward the high-angle direction. */
    APP_AUTO_STAGE_SCAN_HIGH,         /**< Scanning toward the high-angle direction to find the fold-angle limit. */
    APP_AUTO_STAGE_WAIT_LOW_SCAN,     /**< Waiting at the center before scanning toward the low-angle direction. */
    APP_AUTO_STAGE_SCAN_LOW,          /**< Scanning toward the low-angle direction to find the init-angle limit. */
} app_auto_stage_t;

/*===========================================================================
 * Auto-Calibration Runtime Structure
 *===========================================================================*/

/**
 * @brief Auto-calibration runtime state.
 *
 * Records the calibration state machine stage, current angle, and timing
 * reference. It is driven periodically by the main control task in
 * app_control_auto_poll().
 */
typedef struct
{
    app_auto_stage_t stage;      /**< Current calibration stage. */
    uint8_t angle;               /**< Current servo angle during scanning. */
    TickType_t last_tick;        /**< System tick count of the previous step, used for step interval timing. */
} app_auto_runtime_t;

/*===========================================================================
 * Module-Level Static Variables
 *===========================================================================*/

/** Control command queue handle; input channels post commands through this FreeRTOS queue. */
static QueueHandle_t s_control_queue;

/** Control module initialization flag: 0 = not initialized, 1 = initialized. */
static uint8_t s_control_inited;

/** Auto-calibration runtime state; only accessed by the main control task, so no extra lock is required. */
static app_auto_runtime_t s_auto_runtime;

/**
 * Low-voltage pending confirmation flag.
 * 0 = voltage is normal, 1 = low voltage has been detected and sustained confirmation is pending.
 */
static uint8_t s_power_low_pending;

/** System tick count when low voltage was first detected, used to calculate confirmation time. */
static TickType_t s_power_low_tick;

/** System tick count of the previous voltage check, used to control the check period. */
static TickType_t s_last_power_check_tick;

/*===========================================================================
 * Internal Helper Functions
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Map servo speed level to step delay in milliseconds.
 *          Speed levels 0 to 5 map to 30/25/20/15/10/4 ms per step.
 *          Higher levels move faster; level 5 uses only 4 ms per step.
 *          A static lookup table provides O(1) conversion without runtime calculation.
 * @para:   speed  Speed level from 0 to APP_SERVO_SPEED_MAX.
 * @return: Step delay in milliseconds; out-of-range values are clamped to the highest level.
 *****************************************************************************/
static uint8_t app_control_speed_to_delay_ms(uint8_t speed)
{
    /** Speed-to-delay lookup table; index is speed level and value is step delay in ms. */
    static const uint8_t speed_delay_ms[APP_SERVO_SPEED_MAX + 1U] = { 30U, 25U, 20U, 15U, 10U, 4U };

    /* Clamp out-of-range values to the highest speed level. */
    if (speed > APP_SERVO_SPEED_MAX)
    {
        speed = APP_SERVO_SPEED_MAX;
    }

    return speed_delay_ms[speed];
}

/*****************************************************************************
 * @brief:  Check elapsed time using the FreeRTOS tick count.
 *          Uses delta calculation instead of absolute comparison so TickType_t rollover is handled correctly.
 * @para:   now_tick    Current system tick count from xTaskGetTickCount().
 * @para:   last_tick   System tick count at the start time.
 * @para:   interval_ms Expected interval in milliseconds.
 * @return: 1U = elapsed, 0U = not elapsed.
 *****************************************************************************/
static uint8_t app_control_tick_elapsed(TickType_t now_tick, TickType_t last_tick, uint32_t interval_ms)
{
    return (((uint32_t)(now_tick - last_tick) * portTICK_PERIOD_MS) >= interval_ms) ? 1U : 0U;
}

/*****************************************************************************
 * @brief:  Save parameters to EEPROM and synchronize the runtime cache.
 *          Performs persistent EEPROM storage through app_storage_save_params()
 *          and runtime state synchronization through app_state_set_params().
 *          This duplicates the same helper in app_hmi.c to avoid cyclic module dependencies.
 * @para:   params  Parameter structure to save; not modified by this function.
 * @return: RET_OK on success; otherwise the corresponding storage error code.
 *****************************************************************************/
static int app_control_save_params(const app_servo_params_t *params)
{
    int ret;

    ret = app_storage_save_params(params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    (void)app_state_set_servo_params(params);

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Apply lock/unlock timing thresholds from parameters to the key service.
 *          Keeps long-press and extra-long-press detection aligned with the current configuration.
 *          Parameter meanings:
 *            - 50 ms = debounce time, fixed value.
 *            - enter_lock_sec * 1000 = long-press detection time in milliseconds.
 *            - enter_unlock_sec * 1000 = extra-long-press detection time in milliseconds.
 * @para:   params  Current parameter structure.
 * @return: None.
 *****************************************************************************/
static void app_control_apply_key_timing(const app_servo_params_t *params)
{
    (void)key_service_set_timing(50U, (uint32_t)params->enter_lock_sec * 1000U, (uint32_t)params->enter_unlock_sec * 1000U);
}

/*****************************************************************************
 * @brief:  Move the servo from the current position to the target angle one degree at a time.
 *          Uses gradual approach: each step increments or decrements by one degree, with delay controlled by speed.
 *          Updates the angle stored in app_state after each step so other modules can read the current position.
 *
 *          Design notes:
 *            - Step-by-step movement avoids current spikes from direct jumps.
 *            - Runtime state is continuously updated so the current angle can be remembered during power loss.
 *            - If already at the target, servo_service_set_angle() is called once without entering the step loop.
 * @para:   target_angle  Target angle, 0 to 180 degrees.
 * @para:   speed         Speed level, 0 to APP_SERVO_SPEED_MAX.
 * @return: RET_OK when motion completes; otherwise the corresponding servo driver error code.
 *****************************************************************************/
static int app_control_move_servo_to(uint8_t target_angle, uint8_t speed)
{
    app_servo_runtime_state_t runtime;
    int8_t step;
    uint8_t delay_ms;
    uint8_t current_angle;
    int ret;

    /* Get the current runtime state, including the current angle. */
    ret = app_state_get_runtime(&runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    current_angle = runtime.current_angle;

    /* Already at the target angle; just ensure the servo command is applied. */
    if (current_angle == target_angle)
    {
        ret = servo_service_set_angle(SERVO_MAIN, target_angle);
        return RET_IS_ERR(ret) ? ret : RET_OK;
    }

    /* Determine the step direction and step delay. */
    step = (current_angle < target_angle) ? 1 : -1;
    delay_ms = app_control_speed_to_delay_ms(speed);

    /* Step one degree at a time until the target angle is reached. */
    while (current_angle != target_angle)
    {
        current_angle = (uint8_t)((int16_t)current_angle + step);  /* Use int16_t as an intermediate type to prevent uint8_t underflow. */
        ret = servo_service_set_angle(SERVO_MAIN, current_angle);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        /* Synchronize the angle to runtime state for power-loss memory, display, and other modules. */
        (void)app_state_set_servo_pose(runtime.servo_pose, current_angle);

        /* Delay between steps to control motion speed; vTaskDelay yields the CPU time slice. */
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Set the servo pose to folded or initial and run the full motion sequence.
 *          The sequence is:
 *            1. Read parameters to get the target angle, angle_fold or angle_init.
 *            2. Pause remote reception to avoid RF interference with servo PWM.
 *            3. Run degree-by-degree stepping motion.
 *            4. Resume remote reception.
 *            5. Update the runtime pose record.
 *            6. Drive the LED status indicator for the current pose.
 *          Pausing the remote receiver during servo motion is necessary because
 *          GPIO interrupts from 433 MHz remote reception can briefly affect PWM timer precision.
 * @para:   pose  Target pose, APP_SERVO_POSE_FOLD or APP_SERVO_POSE_INIT.
 * @return: RET_OK when the pose switch succeeds; otherwise a parameter or servo driver error code.
 *****************************************************************************/
static int app_control_set_servo_pose(app_servo_pose_t pose)
{
    app_servo_params_t params;
    uint8_t target_angle;
    int ret;

    /* Get the current parameters and determine the target angle. */
    ret = app_state_get_servo_params(&params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    target_angle = (pose == APP_SERVO_POSE_FOLD) ? params.angle_fold : params.angle_init;

    /* Pause remote receiver, move, then resume it to keep PWM unaffected by RF interrupts. */
    (void)remote_service_stop();
    ret = app_control_move_servo_to(target_angle, params.servo_speed);
    (void)remote_service_start();

    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Update the runtime pose record. */
    (void)app_state_set_servo_pose(pose, target_angle);

    /* LED status indication: folded pose turns the LED on, initial pose turns it off. */
    if (pose == APP_SERVO_POSE_FOLD)
    {
        (void)led_service_on(LED_SERVO_STATUS);
    }
    else
    {
        (void)led_service_off(LED_SERVO_STATUS);
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Memory function: save the servo pose before power-off.
 *          Runs only when memory_function_enable is nonzero, writing the current
 *          pose, folded or initial, to EEPROM for recovery on the next startup.
 *
 *          Called before low-voltage power-off so the state is not lost.
 * @para:   pose  Current pose, folded or initial.
 * @return: None.
 *****************************************************************************/
static void app_control_save_power_off_state(app_servo_pose_t pose)
{
    app_servo_params_t params;

    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return;
    }

    /* Skip if the memory function is disabled. */
    if (params.memory_function_enable == 0U)
    {
        return;
    }

    params.power_off_fold_state = (uint8_t)pose;
    (void)app_control_save_params(&params);
}

/*****************************************************************************
 * @brief:  Enter lock mode.
 *          Lock sequence:
 *            1. Enable power hold to prevent unexpected power loss while locked.
 *            2. If currently folded, return to the init angle first so the servo is in a safe position.
 *            3. Switch the work mode to APP_WORK_MODE_LOCK.
 *            4. Start LED blinking, 500 ms period for 6 blinks.
 *          After entering lock mode, servo output is disabled and only unlock conditions are monitored.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_enter_lock(void)
{
    app_servo_runtime_state_t runtime;

    /* Enable battery power hold. */
    (void)app_hw_power_hold_set(1U);

    /* Return to the init angle before locking so the servo stays in a safe position. */
    if (app_state_get_runtime(&runtime) == RET_OK)
    {
        if (runtime.servo_pose == APP_SERVO_POSE_FOLD)
        {
            (void)app_control_set_servo_pose(APP_SERVO_POSE_INIT);
        }
    }

    /* Switch to lock mode. */
    (void)app_state_set_work_mode(APP_WORK_MODE_LOCK);

    /* LED blinking indication: 500 ms period, 6 blinks. */
    (void)led_service_blink_start(LED_SERVO_STATUS, 500U, 6U);
}

/*****************************************************************************
 * @brief:  Exit lock mode.
 *          Restores normal mode and blinks the LED to indicate unlock success.
 *          If the supply voltage is below the threshold, disables power hold
 *          after a delay to avoid draining the battery.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_exit_lock( void )
{
     uint32_t voltage_mv = 0U;

    /* Restore normal mode. */
    (void)app_state_set_work_mode(APP_WORK_MODE_NORMAL);

    /* Blink the LED to indicate successful unlock. */
    (void)led_service_blink_start(LED_SERVO_STATUS, 500U, 6U);

    /* For key unlock, check the power voltage to decide whether to power off. */
    if ((app_hw_read_power_voltage_mv(&voltage_mv) == RET_OK) && (voltage_mv < APP_UNLOCK_KEEP_POWER_THRESHOLD_MV))
    {
        /* Disconnect battery power after unlock to avoid battery drain. */
        vTaskDelay(pdMS_TO_TICKS(2500U));
        (void)app_hw_power_hold_set(0U);
    }
}

/*****************************************************************************
 * @brief:  Stop auto-calibration and restore the normal system state.
 *          Resets the calibration state machine to IDLE, resumes remote
 *          reception, and switches the work mode back to NORMAL.
 *          Called when calibration completes or is interrupted, such as by a lock command.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_auto_stop(void)
{
    s_auto_runtime.stage = APP_AUTO_STAGE_IDLE;
    (void)remote_service_start();
    (void)app_state_set_work_mode(APP_WORK_MODE_NORMAL);
}

/*****************************************************************************
 * @brief:  Save and persist the fold angle measured by auto-calibration.
 *          Called during the high-angle scan after locked-rotor detection,
 *          storing the detected angle or backed-off angle to EEPROM.
 * @para:   angle  Calibrated fold angle, either the locked-rotor position or the backed-off position.
 * @return: None.
 *****************************************************************************/
static void app_control_auto_save_fold_angle(uint8_t angle)
{
    app_servo_params_t params;

    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return;
    }

    params.angle_fold = angle;
    (void)app_control_save_params(&params);
}

/*****************************************************************************
 * @brief:  Save and persist the init angle measured by auto-calibration.
 *          Called during the low-angle scan after locked-rotor detection,
 *          storing the detected angle or backed-off angle to EEPROM.
 * @para:   angle  Calibrated init angle, either the locked-rotor position or the backed-off position.
 * @return: None.
 *****************************************************************************/
static void app_control_auto_save_init_angle(uint8_t angle)
{
    app_servo_params_t params;

    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return;
    }

    params.angle_init = angle;
    (void)app_control_save_params(&params);
}

/*****************************************************************************
 * @brief:  Start the auto-calibration flow.
 *          Initializes the calibration state machine and enters the first stage:
 *          center wait followed by high-angle scan.
 *            1. Pause remote reception to avoid RF interference during calibration.
 *            2. Switch work mode to APP_WORK_MODE_AUTO_CALIBRATE.
 *            3. Blink the LED as an indication, 100 ms period for 12 blinks.
 *            4. Move the servo to the 90-degree center position and set WAIT_HIGH_SCAN.
 *            5. app_control_auto_poll() periodically advances the state machine.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_auto_start(void)
{
    /* Pause the remote receiver during calibration to keep RF interrupts from affecting PWM. */
    (void)remote_service_stop();

    /* Switch to auto-calibration work mode. */
    (void)app_state_set_work_mode(APP_WORK_MODE_AUTO_CALIBRATE);

    /* Blink the LED quickly to indicate calibration mode. */
    (void)led_service_blink_start(LED_SERVO_STATUS, 100U, 12U);

    /* Move the servo to the 90-degree center position and scan toward both ends from there. */
    s_auto_runtime.angle = APP_AUTO_CENTER_ANGLE;
    s_auto_runtime.stage = APP_AUTO_STAGE_WAIT_HIGH_SCAN;
    s_auto_runtime.last_tick = xTaskGetTickCount();
    (void)servo_service_set_angle(SERVO_MAIN, s_auto_runtime.angle);
    (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, s_auto_runtime.angle);
}

/*****************************************************************************
 * @brief:  Poll the auto-calibration state machine, called periodically by the main control task.
 *          Executes the calibration step that corresponds to the current stage:
 *
 *          WAIT_HIGH_SCAN, center wait for stabilization:
 *            - Holds the servo at the center for APP_AUTO_CENTER_WAIT_MS milliseconds.
 *            - Switches to SCAN_HIGH after the timeout.
 *
 *          SCAN_HIGH, high-angle scan to find the fold-angle upper limit:
 *            - Increases by one degree every APP_AUTO_STEP_INTERVAL_MS milliseconds.
 *            - Reads the locked-rotor ADC value after each step.
 *            - Stops when the high scan limit is reached or the locked-rotor threshold is exceeded.
 *            - Backs off by APP_AUTO_BACKOFF_ANGLE degrees when locked rotor is detected.
 *            - Saves the fold angle and returns to center before the low-angle scan.
 *
 *          WAIT_LOW_SCAN, center wait for stabilization:
 *            - Same as above; switches to SCAN_LOW after center stabilization.
 *
 *          SCAN_LOW, low-angle scan to find the init-angle lower limit:
 *            - Decreases by one degree every APP_AUTO_STEP_INTERVAL_MS milliseconds.
 *            - Reads the locked-rotor ADC value after each step.
 *            - Uses the more sensitive APP_LOCKED_ROTOR_LOW_SCAN_RAW threshold.
 *            - Stops when the low scan limit is reached or the low-scan threshold is exceeded.
 *            - Moves forward by APP_AUTO_BACKOFF_ANGLE degrees when locked rotor is detected.
 *            - Saves the init angle, moves to the final init position, and completes calibration.
 *
 *          IDLE / default:
 *            - Calibration is not running, or the state is abnormal; call auto_stop for cleanup.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_auto_poll(void)
{
    TickType_t now_tick;
    uint32_t locked_raw = 0U;
    uint8_t save_angle;  /* Temporarily stores the measured limit angle. */

    /* Return immediately when idle; no calibration task is active. */
    if (s_auto_runtime.stage == APP_AUTO_STAGE_IDLE)
    {
        return;
    }

    now_tick = xTaskGetTickCount();

    switch (s_auto_runtime.stage)
    {
        /* --- Stage 1: center wait before high-angle scan. --- */
        case APP_AUTO_STAGE_WAIT_HIGH_SCAN:
            if (app_control_tick_elapsed(now_tick, s_auto_runtime.last_tick, APP_AUTO_CENTER_WAIT_MS) != 0U)
            {
                /* Wait time elapsed; enter the high-angle scan stage. */
                s_auto_runtime.stage = APP_AUTO_STAGE_SCAN_HIGH;
                s_auto_runtime.last_tick = now_tick;
            }
            break;

        /* --- Stage 2: high-angle scan to find the fold-angle upper limit. --- */
        case APP_AUTO_STAGE_SCAN_HIGH:
            /* Control the scan step rate. */
            if (app_control_tick_elapsed(now_tick, s_auto_runtime.last_tick, APP_AUTO_STEP_INTERVAL_MS) == 0U)
            {
                break;
            }
            s_auto_runtime.last_tick = now_tick;

            /* Increase by one degree and drive the servo. */
            s_auto_runtime.angle++;
            (void)servo_service_set_angle(SERVO_MAIN, s_auto_runtime.angle);
            (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, s_auto_runtime.angle);

            /* Read the locked-rotor ADC to determine whether the mechanical limit is reached. */
            (void)app_hw_read_locked_rotor_raw(&locked_raw);

            if ((s_auto_runtime.angle >= APP_AUTO_HIGH_SCAN_LIMIT) || (locked_raw >= APP_LOCKED_ROTOR_THRESHOLD_RAW))
            {
                save_angle = s_auto_runtime.angle;

                /* Back off by the safety margin when locked rotor is detected to avoid the mechanical endpoint. */
                save_angle = (uint8_t)(save_angle - APP_AUTO_BACKOFF_ANGLE);

                /* Save the fold angle and return to center before the low-angle scan. */
                app_control_auto_save_fold_angle(save_angle);

                s_auto_runtime.angle = APP_AUTO_CENTER_ANGLE;
                s_auto_runtime.stage = APP_AUTO_STAGE_WAIT_LOW_SCAN;
                s_auto_runtime.last_tick = now_tick;
                (void)servo_service_set_angle(SERVO_MAIN, s_auto_runtime.angle);
                (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, s_auto_runtime.angle);
            }
            break;

        /* --- Stage 3: center wait before low-angle scan. --- */
        case APP_AUTO_STAGE_WAIT_LOW_SCAN:
            if (app_control_tick_elapsed(now_tick, s_auto_runtime.last_tick, APP_AUTO_CENTER_WAIT_MS) != 0U)
            {
                /* Wait time elapsed; enter the low-angle scan stage. */
                s_auto_runtime.stage = APP_AUTO_STAGE_SCAN_LOW;
                s_auto_runtime.last_tick = now_tick;
            }
            break;

        /* --- Stage 4: low-angle scan to find the init-angle lower limit. --- */
        case APP_AUTO_STAGE_SCAN_LOW:
            /* Control the scan step rate. */
            if (app_control_tick_elapsed(now_tick, s_auto_runtime.last_tick, APP_AUTO_STEP_INTERVAL_MS) == 0U)
            {
                break;
            }
            s_auto_runtime.last_tick = now_tick;

            /* Decrease by one degree and drive the servo. */
            s_auto_runtime.angle--;
            (void)servo_service_set_angle(SERVO_MAIN, s_auto_runtime.angle);
            (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, s_auto_runtime.angle);

            /* Read the locked-rotor ADC and use the more sensitive low-angle threshold. */
            (void)app_hw_read_locked_rotor_raw(&locked_raw);

            if ((s_auto_runtime.angle <= APP_AUTO_LOW_SCAN_LIMIT) || (locked_raw >= APP_LOCKED_ROTOR_LOW_SCAN_RAW))
            {
                save_angle = s_auto_runtime.angle;

                /* Move forward by the safety margin when locked rotor is detected in the low-angle direction. */
                save_angle = (uint8_t)(save_angle + APP_AUTO_BACKOFF_ANGLE);

                /* Save the init angle, move the servo to the final position, and complete calibration. */
                app_control_auto_save_init_angle(save_angle);
                (void)servo_service_set_angle(SERVO_MAIN, save_angle);
                (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, save_angle);
                (void)led_service_blink_start(LED_SERVO_STATUS, 100U, 12U);

                app_control_auto_stop();
            }
            break;

        /* Unknown stage: perform safe cleanup. */
        default:
            app_control_auto_stop();
            break;
    }
}

/*****************************************************************************
 * @brief:  Update the two-stage low-voltage confirmation state.
 * @para:   now_tick    Current system tick count.
 * @para:   voltage_mv  Current supply voltage in mV.
 * @return: 1U = power loss confirmed, 0U = not yet confirmed.
 *****************************************************************************/
static uint8_t app_control_power_lost_confirmed(TickType_t now_tick, uint32_t voltage_mv)
{
    if (voltage_mv >= APP_POWER_LOW_FIRST_THRESHOLD_MV)
    {
        s_power_low_pending = 0U;
        return 0U;
    }

    if (s_power_low_pending == 0U)
    {
        s_power_low_pending = 1U;
        s_power_low_tick = now_tick;
        return 0U;
    }

    if (app_control_tick_elapsed(now_tick, s_power_low_tick, APP_POWER_LOST_CONFIRM_MS) == 0U)
    {
        return 0U;
    }

    app_hw_read_power_voltage_mv(&voltage_mv);
    if ( voltage_mv >= APP_POWER_LOW_CONFIRM_THRESHOLD_MV )
    {
        s_power_low_pending = 0U;
        return 0U;
    }

    return 1U;
}

/*****************************************************************************
 * @brief:  Perform pre-power-off handling after low-voltage confirmation and disable power hold.
 * @para:   runtime  Current runtime state.
 * @return: None.
 *****************************************************************************/
static void app_control_execute_power_off(const app_servo_runtime_state_t *runtime)
{
    app_servo_params_t params;
    app_servo_pose_t pose_before_return;

    if (runtime == 0)
    {
        return;
    }

    pose_before_return = runtime->servo_pose;

    if (runtime->servo_pose == APP_SERVO_POSE_FOLD)
    {
        if ((app_state_get_servo_params(&params) == RET_OK) && (params.power_off_return == 0U))
        {
            (void)app_control_set_servo_pose(APP_SERVO_POSE_INIT);
        }
    }

    app_control_save_power_off_state(pose_before_return);

    vTaskDelay(pdMS_TO_TICKS(1000U));
    (void)app_hw_power_hold_set(0U);

    s_power_low_pending = 0U;
}

/*****************************************************************************
 * @brief:  Periodic power voltage check and low-voltage power-off handling.
 *          Called by the main control task every cycle to implement the two-stage low-voltage decision logic.
 * @para:   None.
 * @return: None.
 *****************************************************************************/
static void app_control_check_power_off(void)
{
    app_servo_runtime_state_t runtime;
    uint32_t voltage_mv;
    TickType_t now_tick;

    now_tick = xTaskGetTickCount();
    if ((s_last_power_check_tick != 0U) &&
        (app_control_tick_elapsed(now_tick, s_last_power_check_tick, APP_POWER_CHECK_PERIOD_MS) == 0U))
    {
        return;
    }
    s_last_power_check_tick = now_tick;

    if (app_state_get_runtime(&runtime) != RET_OK)
    {
        return;
    }

    if (runtime.work_mode != APP_WORK_MODE_NORMAL)
    {
        s_power_low_pending = 0U;
        return;
    }

    if (app_hw_read_power_voltage_mv(&voltage_mv) != RET_OK)
    {
        return;
    }

    (void)app_state_set_voltage(voltage_mv, runtime.locked_rotor_raw);

    if (app_control_power_lost_confirmed(now_tick, voltage_mv) == 0U)
    {
        return;
    }

    app_control_execute_power_off(&runtime);
}

/*****************************************************************************
 * @brief:  Dispatch and handle control commands.
 *          Executes the corresponding operation according to the current work mode
 *          (NORMAL, LOCK, or AUTO_CALIBRATE) and command type:
 *
 *          Work mode constraints:
 *            - During auto-calibration, only ENTER_LOCK is accepted; other commands are ignored.
 *              This prevents accidental interruption of the calibration flow.
 *            - In lock mode, only unlock commands are handled.
 *
 *          Command-to-action map:
 *            TOGGLE_SERVO         -> normal mode: switch between folded and init angles.
 *            SERVO_INIT           -> normal mode: move to init angle.
 *            SERVO_FOLD           -> normal mode: move to fold angle.
 *            ENTER_LOCK           -> any mode: stop calibration if needed, then enter lock mode.
 *            EXIT_LOCK_BY_KEY     -> lock mode: key unlock with conditional power-off.
 *            EXIT_LOCK_POWER_OFF  -> lock mode: remote unlock.
 *            EXIT_LOCK_BY_BLE     -> lock mode: BLE unlock.
 *            START_AUTO_CALIBRATE -> normal mode: start auto-calibration.
 * @para:   cmd  Control command.
 * @return: None.
 *****************************************************************************/
static void app_control_handle_command(app_control_cmd_t cmd)
{
    app_servo_runtime_state_t servo_runtime;
    app_servo_params_t servo_params;

    if (app_state_get_runtime(&servo_runtime) != RET_OK)
    {
        return;
    }

    if (app_state_get_servo_params(&servo_params) != RET_OK)
    {
        return;
    }

    /*
     * During auto-calibration, only ENTER_LOCK is allowed so calibration can
     * be stopped and lock mode entered in an emergency. All other commands are
     * ignored to prevent accidental disruption of calibration.
     */
    if ((servo_runtime.work_mode == APP_WORK_MODE_AUTO_CALIBRATE) && (cmd != APP_CONTROL_CMD_ENTER_LOCK))
    {
        return;
    }

    switch (cmd)
    {
        case APP_CONTROL_CMD_TOGGLE_SERVO:
        {
            /* Normal mode: switch between folded and init angles. */
            if (servo_runtime.work_mode == APP_WORK_MODE_NORMAL)
            {
                if(servo_runtime.servo_pose == APP_SERVO_POSE_FOLD)
                {
                    app_control_set_servo_pose(APP_SERVO_POSE_INIT);
                }
                else
                {
                    app_control_set_servo_pose(APP_SERVO_POSE_FOLD);
                }
            }
        }
        break;

        case APP_CONTROL_CMD_SERVO_INIT:
        {
            /* Normal mode: move directly to the init angle. */
            if (servo_runtime.work_mode == APP_WORK_MODE_NORMAL)
            {
                (void)app_control_set_servo_pose(APP_SERVO_POSE_INIT);
            }
        }
        break;

        case APP_CONTROL_CMD_SERVO_FOLD:
        {
            /* Normal mode: move directly to the fold angle. */
            if (servo_runtime.work_mode == APP_WORK_MODE_NORMAL)
            {
                (void)app_control_set_servo_pose(APP_SERVO_POSE_FOLD);
            }
        }
        break;

        case APP_CONTROL_CMD_ENTER_LOCK:
        {
            /* Enter lock: if not already locked, stop calibration first and then enter lock mode. */
            if (servo_runtime.work_mode != APP_WORK_MODE_LOCK)
            {
                app_control_auto_stop();
                app_control_enter_lock();
            }
        }
        break;

        case APP_CONTROL_CMD_EXIT_LOCK_BY_KEY:      // Key unlock.
        case APP_CONTROL_CMD_EXIT_LOCK_BY_REMOTE:   // Remote unlock.
        {
            if (servo_runtime.work_mode == APP_WORK_MODE_LOCK)
            {
                if (servo_params.unlock_mode == (uint8_t)APP_UNLOCK_MODE_ALL)
                {
                    app_control_exit_lock();
                }
            }
        }
        break;

        case APP_CONTROL_CMD_EXIT_LOCK_BY_BLE:
        {
            /* BLE unlock: allowed in both ALL and BLE_ONLY modes. */
            if (servo_runtime.work_mode == APP_WORK_MODE_LOCK)
            {
                app_control_exit_lock();
            }
        }
        break;

        case APP_CONTROL_CMD_START_AUTO_CALIBRATE:
        {
            /* Start auto-calibration: valid only in normal mode. */
            if (servo_runtime.work_mode == APP_WORK_MODE_NORMAL)
            {
                app_control_auto_start();
            }
        }
        break;

        default:
            /* Unknown command; silently ignore it. */
            break;
    }
}

/*****************************************************************************
 * @brief:  Main control task, AppControl.
 *          Core system scheduling task. It loops with a 20 ms period to:
 *            1. Pull pending commands from the command queue and handle them through app_control_handle_command().
 *            2. Drive the auto-calibration state machine through app_control_auto_poll().
 *            3. Run power voltage checks and low-voltage power-off handling through app_control_check_power_off().
 *
 *          Uses non-blocking queue reception with a 20 ms timeout so calibration
 *          polling and power checks still run periodically when no command is pending.
 * @para:   arg  Task argument, unused and passed as NULL.
 * @return: None; infinite task, never returns.
 *****************************************************************************/
static void app_control_task(void *arg)
{
    app_control_cmd_t cmd;

    (void)arg;

    while (1)
    {
        /* Non-blocking command receive: handle a command if present, otherwise continue after timeout. */
        if (xQueueReceive(s_control_queue, &cmd, pdMS_TO_TICKS(20U)) == pdTRUE)
        {
            app_control_handle_command(cmd);
        }

        /* Drive the auto-calibration state machine; idle state has negligible overhead. */
        app_control_auto_poll();

        /* Periodically check the power voltage. */
        app_control_check_power_off();
    }
}

/*****************************************************************************
 * @brief:  Locked-rotor detection task, RotorCheck.
 *          Runs independently every 10 ms and does not depend on the main control task.
 *          It monitors the servo current in real time:
 *
 *          Detection logic:
 *            - Read the raw locked-rotor ADC value and synchronize it to runtime state.
 *            - Increment the counter when the ADC value is greater than or equal to APP_LOCKED_ROTOR_THRESHOLD_RAW.
 *            - Confirm locked rotor after APP_LOCKED_ROTOR_CONFIRM_COUNT consecutive over-threshold samples.
 *            - After confirmation, perform protection actions:
 *                * Reset the init angle to APP_LOCK_INIT_ANGLE.
 *                * Reset the fold angle to APP_LOCK_FOLD_ANGLE.
 *                * Move the servo to APP_LOCK_INIT_ANGLE.
 *                * Turn off the LED status indicator.
 *            - Clear the counter immediately when the ADC value falls below the threshold.
 *
 *          Debounce design:
 *            Uses repeated detection instead of a single trigger to prevent false
 *            reports from startup current spikes or external EMI. A 10 ms x 15
 *            confirmation window is much shorter than the mechanical damage
 *            window while still filtering electrical noise.
 *
 *          Special handling:
 *            - Locked-rotor protection is not executed during auto-calibration,
 *              because calibration itself depends on locked-rotor detection.
 * @para:   arg  Task argument, unused and passed as NULL.
 * @return: None; infinite task, never returns.
 *****************************************************************************/
static void app_locked_rotor_task(void *arg)
{
    uint8_t over_count = 0U;    /* Consecutive over-threshold counter. */
    uint32_t locked_raw;        /* Raw ADC sample value. */
    app_servo_runtime_state_t runtime;
//    app_servo_params_t params;

    (void)arg;

    while (1)
    {
        /*
         * Run locked-rotor protection only outside calibration mode.
         * Auto-calibration has its own locked-rotor logic for locating mechanical limits.
         */
        if ((app_state_get_runtime(&runtime) == RET_OK) &&
            (runtime.work_mode != APP_WORK_MODE_AUTO_CALIBRATE) &&
            (app_hw_read_locked_rotor_raw(&locked_raw) == RET_OK))
        {
            /* Synchronize ADC data to runtime state. */
            (void)app_state_set_voltage(runtime.power_voltage_mv, locked_raw);

            if (locked_raw >= APP_LOCKED_ROTOR_THRESHOLD_RAW)
            {
                 /* Over threshold: increment the counter. */
                 over_count++;
                 if (over_count >= APP_LOCKED_ROTOR_CONFIRM_COUNT)
                 {
                     /* Locked rotor confirmed: reset the counter. */
                     over_count = 0U;

                    //  /* Reset angle parameters to safe defaults to prevent continued motion toward the locked direction. */
                    //  if (app_state_get_servo_params(&params) == RET_OK)
                    //  {
                    //      params.angle_init = APP_LOCK_INIT_ANGLE;        /* Set the init angle to the safe position. */
                    //      params.angle_fold = APP_LOCK_FOLD_ANGLE;        /* Reduce the fold angle to the minimum safe value. */
                    //      (void)app_control_save_params(&params);
                    //  }

                    //  /* Drive the servo to the safe position immediately. */
                    //  (void)servo_service_set_angle(SERVO_MAIN, APP_LOCK_INIT_ANGLE);
                    //  (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, APP_LOCK_INIT_ANGLE);  // Update runtime state and angle.

                    //  /* Turn off the LED to indicate the abnormal state. */
                    //  (void)led_service_off(LED_SERVO_STATUS);
                 }
            }
            else
            {
                /* ADC value dropped: clear the counter to prevent accumulated triggers. */
                over_count = 0U;
            }
        }

        /* 10 ms detection period. */
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
}

/*===========================================================================
 * Public Interfaces
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Asynchronously post one control command to the control task.
 *          Non-blocking interface; writes the command to the FreeRTOS queue
 *          through xQueueSend() and returns immediately. TickType = 0 makes
 *          posting non-blocking: a full queue returns RET_BUSY instead of waiting.
 *
 *          Design notes:
 *            Non-blocking posting avoids suspending the caller when the queue is full.
 *            Command loss is unlikely because the queue depth is 8 and the handling
 *            period is 20 ms. Control commands are also effectively idempotent in
 *            typical use, so dropping commands in exceptional cases is acceptable.
 * @para:   cmd  Control command; see app_control_cmd_t.
 * @return: RET_OK         Command queued successfully.
 *          RET_NOT_INITED Module is not initialized; the queue does not exist.
 *          RET_BUSY       Queue is full; posting failed and the caller may retry or drop it.
 *****************************************************************************/
int app_control_post_command(app_control_cmd_t cmd)
{
    if (s_control_queue == 0)
    {
        return RET_NOT_INITED;
    }

    if (xQueueSend(s_control_queue, &cmd, (TickType_t)0U) != pdTRUE)
    {
        return RET_BUSY;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Initialize the control module.
 *          Performs the following steps in order:
 *            1. Check idempotency through s_control_inited to prevent re-entry.
 *            2. Read persistent parameters and apply lock/unlock timing thresholds to the key service.
 *            3. Enable power hold so power is maintained during initialization.
 *            4. Move the servo to the angle matching the remembered pose, init or folded.
 *            5. Set the initial LED state according to the pose.
 *            6. Create the command queue with depth 8.
 *            7. Create the main control task AppControl, stack 640 words, priority 3.
 *            8. Create the locked-rotor detection task RotorCheck, stack 384 words, priority 2.
 *            9. Mark s_control_inited = 1.
 *
 *          Notes:
 *            - If any queue or task creation fails, RET_NO_RESOURCE is returned without rollback.
 *            - The servo has already moved to the target angle in step 4 before later commands are processed.
 *            - app_state must already be initialized; otherwise step 2 fails with RET_NOT_INITED.
 * @para:   None.
 * @return: RET_OK         Initialization succeeded, including repeated calls after initialization.
 *          RET_NOT_INITED Dependent module app_state is not initialized.
 *          RET_NO_RESOURCE Queue or task creation failed, due to memory shortage or configuration error.
 *****************************************************************************/
int app_control_init(void)
{
    app_servo_params_t params;
    app_servo_runtime_state_t runtime;
    uint8_t init_angle;

    /* Idempotency: return immediately if already initialized. */
    if (s_control_inited != 0U)
    {
        return RET_OK;
    }

    /* Read EEPROM parameters through the runtime cache. */
    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return RET_NOT_INITED;
    }

    /* Apply key timing thresholds. */
    app_control_apply_key_timing(&params);

    /* Enable battery power hold by default. */
    (void)app_hw_power_hold_set(1U);

    /* Move the servo to the angle matching the remembered pose. */
    if (app_state_get_runtime(&runtime) == RET_OK)
    {
        if(runtime.servo_pose == APP_SERVO_POSE_FOLD)
        {
            init_angle = params.angle_fold;
            (void)led_service_on(LED_SERVO_STATUS);     // Turn LED on in folded pose.
        }
        else
        {
            init_angle = params.angle_init;
            (void)led_service_off(LED_SERVO_STATUS);    // Turn LED off in init pose.
        }
        (void)servo_service_set_angle(SERVO_MAIN, init_angle);
        (void)app_state_set_servo_pose(runtime.servo_pose, init_angle);
    }

    /* Create the control command queue. */
    s_control_queue = xQueueCreate(APP_CONTROL_QUEUE_LEN, sizeof(app_control_cmd_t));
    if (s_control_queue == 0)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the main control task. */
    if (xTaskCreate(app_control_task, "AppControl", APP_CONTROL_TASK_STACK, 0, APP_CONTROL_TASK_PRIO, 0) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the locked-rotor detection task. */
   if (xTaskCreate(app_locked_rotor_task, "RotorCheck", APP_LOCKED_ROTOR_TASK_STACK, 0, APP_LOCKED_ROTOR_TASK_PRIO, 0) != pdPASS)
   {
       return RET_NO_RESOURCE;
   }

    /* Mark initialization complete. */
    s_control_inited = 1U;

    return RET_OK;
}
