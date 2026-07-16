/*****************************************************************************
 * @file    app_hmi.c
 * @brief   HMI (human-machine interface) module implementation.
 *          Acts as the human-machine interaction hub of the servo controller
 *          and centrally manages the following input channels:
 *            - Physical keys (short press, long press, very long press)
 *            - Rotary encoder (clockwise and counterclockwise rotation)
 *            - 433 MHz remote controller (unlock, mode toggle, lock)
 *            - Bluetooth UART (ASCII control commands and binary OTA
 *              GET_INFO / ENTER_OTA requests)
 *          Each channel runs as an independent FreeRTOS task. Shared state is
 *          protected by a mutex, and parsed commands are dispatched through
 *          queues to modules such as app_control and app_display.
 *
 *  Bluetooth ASCII command protocol (single-character command plus optional
 *  parameter value):
 *    'a'      Start automatic calibration.
 *    'b' <n>  Set the initial angle (APP_PARAM_ANGLE_INIT_MIN ~ MAX).
 *    'c' <n>  Set the folded angle (APP_PARAM_ANGLE_FOLD_MIN ~ 180).
 *    'd' <n>  Set the servo rotation speed (1 ~ APP_PARAM_SERVO_SPEED_MAX).
 *    'e' <n>  Set the power-off return switch (0=return, 1=no return).
 *    'f' <n>  Set the lock-entry delay (APP_PARAM_LOCK_SEC_MIN ~ MAX).
 *    'g' <n>  Set the unlock-entry delay (APP_PARAM_UNLOCK_SEC_MIN ~ MAX).
 *    'h'      Report all current parameters through Bluetooth UART.
 *    'i'      Toggle the servo between folded and unfolded states.
 *    'j'      Set unlock mode to "all sources allowed".
 *    'k'      Set unlock mode to "Bluetooth only".
 *    'l'      Enter the locked state.
 *    'm'      Exit the locked state with power-off return.
 *    'n' <n>  Set the memory function switch (0=disabled, 1=enabled).
 * @author  GD32 Servo Control Team
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#include "app_hmi.h"

/* Application-layer module interfaces */
#include "app_control.h"        /* Servo control command posting */
#include "app_display.h"        /* Display edit mode and value adjustment */
#include "app_hw.h"             /* Hardware abstraction layer, such as Bluetooth UART I/O */
#include "app_state.h"          /* Global system parameter access */
#include "app_storage.h"        /* Persistent parameter storage */
#include "dev_uart.h"           /* UART device driver, frame length, and timeout definitions */
#include "encoder_service.h"    /* Encoder event abstraction layer */
#include "feature_config.h"
#include "key_service.h"        /* Key event abstraction layer for short, long, and very long presses */
#if CONFIG_USE_OTA
#include "ota_service.h"        /* App-side GET_INFO and ENTER_OTA protocol service */
#endif
#include "remote_service.h"     /* Remote controller event abstraction layer */
#include "return_code.h"        /* Common return code definitions, such as RET_OK / RET_IS_OK / RET_IS_ERR */

/* FreeRTOS kernel components */
#include "FreeRTOS.h"
#include "semphr.h"        /* Mutex */
#include "task.h"          /* Task creation */

/* C standard library */
#include <stdarg.h>        /* va_list / va_start / va_end for variadic formatted output */
#include <stddef.h>        /* offsetof */
#include <stdint.h>        /* Fixed-width integer types, such as uint8_t / uint32_t */
#include <stdio.h>         /* vsnprintf string formatting */
#include <string.h>        /* memset memory initialization */

/*===========================================================================
 * Macro definitions - task configuration
 *===========================================================================*/

/** Stack depth of AppKey / AppEncoder / AppRemote tasks, in words (4 bytes per word). */
#define APP_HMI_TASK_STACK                  512U

/** Stack depth of the AppBT Bluetooth UART task, in words (4 bytes per word).
 *  The Bluetooth task parses ASCII strings and needs more stack space for
 *  temporary buffers. */
#define APP_BT_TASK_STACK                   768U

/** Common HMI task priority; lower numeric values mean lower priority. */
#define APP_HMI_TASK_PRIO                   2U

/** Bluetooth task priority, higher than other HMI tasks for timely command response. */
#define APP_BT_TASK_PRIO                    3U

/*===========================================================================
 * Macro definitions - remote controller command codes
 *===========================================================================*/

/** Third byte of the remote data frame: unlock command with power-off return. */
#define APP_REMOTE_UNLOCK                   0x08U

/** Third byte of the remote data frame: toggle servo mode (folded <-> unfolded). */
#define APP_REMOTE_SWITCH_MODE              0x04U

/** Third byte of the remote data frame: lock command. */
#define APP_REMOTE_LOCK                     0x02U

/*===========================================================================
 * Macro definitions - timeouts and buffers
 *===========================================================================*/

/** Remote pairing timeout, in milliseconds (10 seconds).
 *  If no matching remote signal is received within this interval after
 *  entering pairing mode, pairing mode is exited automatically. */
#define APP_REMOTE_PAIR_TIMEOUT_MS          10000U

/** Bluetooth receive buffer size, reusing the maximum frame length from the UART driver. */
#define APP_BT_RX_BUF_LEN                   DEV_UART_FRAME_MAX_LEN

/** Bluetooth transmit buffer size, in bytes.
 *  Used for vsnprintf formatted output and sized to hold the full parameter
 *  report text. */
#define APP_BT_TX_BUF_LEN                   160U

/** 蓝牙 OTA 启动命令。使用 AT 风格命令，避免与原有单字符命令冲突。 */
#if CONFIG_USE_OTA
#define APP_BT_OTA_START_CMD                "AT+OTA=START"
#endif


#define APP_DISPLAY_ENCODER_VALUE_ADD       1U
#define APP_DISPLAY_ENCODER_VALUE_REDUCE    -1


/*===========================================================================
 * Type definitions
 *===========================================================================*/

/**
 * @brief Remote pairing state.
 *
 * Records whether remote pairing mode is active and the tick count when the
 * mode was entered. This enables automatic exit when pairing times out.
 * Access to this structure must be protected by s_hmi_mutex.
 */
typedef struct
{
    uint8_t pairing;            /**< Pairing state: 0=not pairing, 1=pairing. */
    TickType_t pair_start_tick; /**< System tick count when pairing mode was entered. */
} app_remote_pair_state_t;

/**
 * @brief Bluetooth parameter-setting command mapping entry.
 */
typedef struct
{
    uint8_t command;            /**< Bluetooth command character. */
    uint8_t min_value;          /**< Minimum allowed parameter value. */
    uint8_t max_value;          /**< Maximum allowed parameter value. */
    size_t field_offset;        /**< Offset of the target uint8_t field in app_servo_params_t. */
    uint8_t update_key_timing;  /**< Whether to synchronize key long/very-long press thresholds after saving. */
} app_bt_param_command_t;

typedef void (*app_key_action_fn_t)(const key_event_t *event);

typedef struct
{
    key_id_t key_id;
    key_press_type_t press_type;
    app_key_action_fn_t action;
} app_key_action_item_t;

/*===========================================================================
 * Module-scope static variables
 *===========================================================================*/

/**
 * @brief HMI module mutex.
 *
 * Protects module-scope shared state such as s_remote_pair and prevents data
 * races caused by concurrent task access. Every code path that reads or writes
 * s_remote_pair must acquire this mutex first.
 */
static SemaphoreHandle_t s_hmi_mutex;

/** Remote pairing state instance, protected by s_hmi_mutex. */
static app_remote_pair_state_t s_remote_pair;

/**
 * @brief Module initialization flag.
 *
 * 0 = not initialized, 1 = initialized.
 * app_hmi_init() uses this flag to remain idempotent and to prevent duplicate
 * task and mutex creation.
 */
static uint8_t s_hmi_inited;

/*===========================================================================
 * Internal helper functions
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Test elapsed time using the FreeRTOS tick count.
 *          Non-blockingly checks whether interval_ms milliseconds have elapsed
 *          since last_tick. Unlike direct system-time comparisons, the
 *          subtraction-based calculation correctly handles TickType_t rollover.
 * @para:   now_tick    Current system tick count, usually from xTaskGetTickCount()
 * @para:   last_tick   System tick count at the start time
 * @para:   interval_ms Expected interval in milliseconds
 * @return: 1U = timed out (elapsed time >= interval_ms), 0U = not timed out
 *****************************************************************************/
static uint8_t app_hmi_tick_elapsed(TickType_t now_tick, TickType_t last_tick, uint32_t interval_ms)
{
    return (((uint32_t)(now_tick - last_tick) * portTICK_PERIOD_MS) >= interval_ms) ? 1U : 0U;
}

/*****************************************************************************
 * @brief:  Acquire the HMI mutex, blocking indefinitely.
 *          This function must be called before reading or writing shared data
 *          such as s_remote_pair. app_hmi_unlock() must be called after the
 *          operation completes.
 * @para:   None
 * @return: RET_OK            Mutex acquired successfully
 *          RET_NOT_INITED     Mutex has not been created, so the module is not initialized
 *          RET_TIMEOUT        Mutex acquisition timed out; theoretically impossible with portMAX_DELAY
 *****************************************************************************/
static int app_hmi_lock(void)
{
    if (s_hmi_mutex == NULL)
    {
        return RET_NOT_INITED;
    }

    if (xSemaphoreTake(s_hmi_mutex, portMAX_DELAY) != pdTRUE)
    {
        return RET_TIMEOUT;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Release the HMI mutex.
 *          Used as the counterpart to app_hmi_lock().
 *          The caller must ensure that the mutex was acquired successfully;
 *          this function does not validate that condition.
 * @para:   None
 * @return: None
 *****************************************************************************/
static void app_hmi_unlock(void)
{
    (void)xSemaphoreGive(s_hmi_mutex);
}

/*****************************************************************************
 * @brief:  Set the remote pairing state.
 *          Updates the pairing flag and start timestamp under mutex protection
 *          to keep concurrent task access safe.
 *          Typical call scenarios:
 *            - enable=1 when the user presses the onboard pairing key
 *            - enable=0 after successful pairing
 *            - enable=0 when app_hmi_is_pairing() detects a timeout
 * @para:   enable  0 = exit pairing mode, nonzero = enter pairing mode
 * @return: None
 *****************************************************************************/
static void app_hmi_set_pairing(uint8_t enable)
{
    if (app_hmi_lock() != RET_OK)
    {
        return;
    }

    s_remote_pair.pairing = enable;
    s_remote_pair.pair_start_tick = xTaskGetTickCount();    // Record the system time when the pairing key is pressed.
    app_hmi_unlock();
}

/*****************************************************************************
 * @brief:  Query whether remote pairing mode is active, including timeout handling.
 *          Reads the pairing state under mutex protection and checks whether
 *          pairing has timed out. If pairing mode is active and
 *          APP_REMOTE_PAIR_TIMEOUT_MS has elapsed, the pairing flag is cleared
 *          automatically and later calls return 0.
 * @para:   None
 * @return: 1U = pairing mode is active and not timed out, 0U = not pairing or timed out
 *****************************************************************************/
static uint8_t app_hmi_is_pairing(void)
{
    uint8_t pairing = 0U;
    TickType_t now_tick;
    int ret = 0;

    if (app_hmi_lock() != RET_OK)
    {
        return 0U;
    }

    now_tick = xTaskGetTickCount();     // Get the current system time.
    ret = app_hmi_tick_elapsed(now_tick, s_remote_pair.pair_start_tick, APP_REMOTE_PAIR_TIMEOUT_MS);    // Check whether pairing has timed out.
    if ((s_remote_pair.pairing != 0U) &&( ret != 0U))
    {
        /* Pairing timed out; exit pairing mode automatically. */
        s_remote_pair.pairing = 0U;
    }

    pairing = s_remote_pair.pairing;
    app_hmi_unlock();

    return pairing;
}

/*****************************************************************************
 * @brief:  Save a parameter set to persistent storage and update runtime state.
 *          Performs two steps:
 *            1. Calls app_storage_save_params() to write parameters to EEPROM.
 *            2. Calls app_state_set_params() to synchronize parameters into
 *               the runtime cache.
 *          If update_key_timing is nonzero, the key-service timing parameters
 *          (lock/unlock trigger times) are also synchronized so that key
 *          long-press thresholds match the current parameters.
 * @para:   params            Pointer to the parameter structure to save
 * @para:   update_key_timing  0 = save parameters only; nonzero = also update key long/very-long press thresholds
 * @return: RET_OK        Save succeeded
 *          Other         Corresponding error code when storage write fails
 *****************************************************************************/
static int app_hmi_save_params(app_servo_params_t *params, uint8_t update_key_timing)
{
    int ret;

    /* Step 1: write to EEPROM for persistence. */
    ret = app_storage_save_params(params);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* Step 2: synchronize to the runtime state cache. */
    (void)app_state_set_servo_params(params);

    /* Update key timing thresholds if requested; these affect long/very-long press detection. */
    if (update_key_timing != 0U)
    {
        (void)key_service_set_timing(50U, (uint32_t)params->enter_lock_sec * 1000U, (uint32_t)params->enter_unlock_sec * 1000U);
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Parse a uint8_t parameter value from a Bluetooth ASCII data frame.
 *          Frame format: the first byte is the command character (buf[0]);
 *          subsequent bytes are the ASCII decimal parameter string.
 *          Parsing rules:
 *            - Skip space, carriage return ('\r'), and line feed ('\n').
 *            - Accept only '0' through '9'; any other character is an error.
 *            - Abort immediately if the intermediate value exceeds max_value.
 *            - Validate the final value against [min_value, max_value].
 *          Special case: when max_value is 180 and the parsed value is 180,
 *          clamp the result to 179. This follows the actual servo angle
 *          limitation, where 180 degrees is not a valid position.
 * @para:   buf       Data frame buffer; buf[0] is the command and buf[1..len-1] is the parameter string
 * @para:   len       Valid buffer length in bytes; at least 2 bytes are required (1 command + 1 digit)
 * @para:   min_value Minimum allowed parameter value
 * @para:   max_value Maximum allowed parameter value
 * @para:   value     [out] Parsed value when successful
 * @return: RET_OK              Parse succeeded
 *          RET_INVALID_PARAM    Invalid parameter, such as null pointer, short length, no valid digit, or out of range
 *****************************************************************************/
static int app_hmi_parse_u8_param(const uint8_t *buf, uint32_t len, uint8_t min_value, uint8_t max_value, uint8_t *value)
{
    uint16_t temp = 0U;       /* Intermediate accumulated value; uint16_t prevents 8-bit overflow. */
    uint32_t i;
    uint8_t has_digit = 0U;   /* Whether at least one valid digit has been parsed. */

    /* Pre-check parameter validity. */
    if ((buf == NULL) || (value == NULL) || (len < 2U))
    {
        return RET_INVALID_PARAM;
    }

    /* Iterate over parameter bytes, skipping buf[0], which is the command character. */
    for (i = 1U; i < len; i++)
    {
        /* Skip whitespace and line endings commonly appended by Bluetooth terminals. */
        if ((buf[i] == '\r') || (buf[i] == '\n') || (buf[i] == ' '))
        {
            continue;
        }

        /* Reject non-digit characters immediately. */
        if ((buf[i] < '0') || (buf[i] > '9'))
        {
            return RET_INVALID_PARAM;
        }

        has_digit = 1U;
        temp = (uint16_t)(temp * 10U + (uint16_t)(buf[i] - '0'));

        /* Stop immediately on intermediate overflow to avoid processing an invalid large value. */
        if (temp > max_value)
        {
            return RET_INVALID_PARAM;
        }
    }

    /* Reject empty numeric input or a value below the minimum. */
    if ((has_digit == 0U) || (temp < min_value))
    {
        return RET_INVALID_PARAM;
    }

    /* Special boundary correction for angle parameters: disallow 180 when max_value is 180. */
    if ((max_value == 180U) && (temp == 180U))
    {
        temp = APP_PARAM_ANGLE_FOLD_MAX;
    }

    *value = (uint8_t)temp;

    return RET_OK;
}

/*****************************************************************************
 * @brief:  Send a formatted string through Bluetooth UART, similar to printf.
 *          Uses vsnprintf internally and sends the result through the hardware
 *          abstraction function app_hw_bluetooth_write().
 *          Notes:
 *            - Output longer than APP_BT_TX_BUF_LEN is truncated automatically.
 *            - Formatting failure, indicated by a negative return value, is
 *              ignored silently and no data is sent.
 * @para:   fmt  printf-style format string
 * @para:   ...  Variable argument list
 * @return: None
 *****************************************************************************/
static void app_hmi_bt_printf(const char *fmt, ...)
{
    char tx_buf[APP_BT_TX_BUF_LEN];  /* Transmit buffer allocated on the stack. */
    va_list args;
    int len;

    /* Format variadic arguments into the buffer. */
    va_start(args, fmt);
    len = vsnprintf(tx_buf, sizeof(tx_buf), fmt, args);
    va_end(args);

    if (len <= 0)
    {
        return;
    }

    /* If the formatted result exceeds the buffer, truncate to the actual capacity. */
    if ((uint32_t)len >= sizeof(tx_buf))
    {
        len = (int)sizeof(tx_buf) - 1;
    }

    /* Send through the hardware abstraction layer. */
    (void)app_hw_bluetooth_write((const uint8_t *)tx_buf, (uint32_t)len, DEV_UART_DEFAULT_TIMEOUT_TICKS);
}

/*****************************************************************************
 * @brief:  Report all current system parameters through Bluetooth UART.
 *          Outputs, in order: initial angle, folded angle, rotation speed,
 *          power-off return policy, lock-entry time, unlock-entry time, memory
 *          function state, and supported unlock methods after locking.
 *          Intended for querying the current configuration with the 'h' command
 *          from a connected Bluetooth debug terminal.
 * @para:   None
 * @return: None
 *****************************************************************************/
static void app_hmi_bt_report_params(void)
{
    app_servo_params_t params;

    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return;
    }

    app_hmi_bt_printf("初始角度:%d\r\n", params.angle_init);
    app_hmi_bt_printf("折叠角度:%d\r\n", params.angle_fold);
    app_hmi_bt_printf("旋转速度:%d\r\n", params.servo_speed);
    app_hmi_bt_printf("%s\r\n", (params.power_off_return == 0U) ? "断电回位" : "断电不回位");
    app_hmi_bt_printf("上锁时间:%d秒\r\n", params.enter_lock_sec);
    app_hmi_bt_printf("解锁时间:%d秒\r\n", params.enter_unlock_sec);
    app_hmi_bt_printf("%s\r\n", (params.memory_function_enable != 0U) ? "记忆功能已开启" : "记忆功能关闭");
    app_hmi_bt_printf("锁定后支持按键超长按、遥控器、蓝牙解锁\r\n");
}

/*===========================================================================
 * FreeRTOS task functions
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Key event action functions.
 *****************************************************************************/
static void app_key_action_toggle_servo(const key_event_t *event)
{
    (void)event;
    (void)app_control_post_command(APP_CONTROL_CMD_TOGGLE_SERVO);
}

static void app_key_action_enter_lock(const key_event_t *event)
{
    (void)event;
    (void)app_control_post_command(APP_CONTROL_CMD_ENTER_LOCK);
}

static void app_key_action_exit_lock_by_key(const key_event_t *event)
{
    (void)event;
    (void)app_control_post_command(APP_CONTROL_CMD_EXIT_LOCK_BY_KEY);
}

static void app_key_action_cycle_edit_mode(const key_event_t *event)
{
    (void)event;
    (void)app_display_cycle_edit_mode();
}

static void app_key_action_start_pairing(const key_event_t *event)
{
    (void)event;
    app_hmi_set_pairing(1U);
    (void)app_hw_bluetooth_write_text("遥控器对码开始\r\n");
}

static const app_key_action_item_t s_key_action_table[] =
{
    {KEY_BUTTON,  KEY_PRESS_SHORT,     app_key_action_toggle_servo      },
    {KEY_BUTTON,  KEY_PRESS_LONG,      app_key_action_enter_lock        },
    {KEY_BUTTON,  KEY_PRESS_VERY_LONG, app_key_action_exit_lock_by_key  },
    {KEY_ENCODER, KEY_PRESS_SHORT,     app_key_action_cycle_edit_mode   },
    {KEY_ONBOARD, KEY_PRESS_SHORT,     app_key_action_start_pairing     },
};

#define APP_KEY_ACTION_TABLE_SIZE ((uint8_t)(sizeof(s_key_action_table) / sizeof(s_key_action_table[0])))

static void app_key_dispatch_event(const key_event_t *event)
{
    uint8_t i;

    if (event == NULL)
    {
        return;
    }

    for (i = 0U; i < APP_KEY_ACTION_TABLE_SIZE; i++)
    {
        if ((s_key_action_table[i].key_id == event->key_id) &&
            (s_key_action_table[i].press_type == event->press_type) &&
            (s_key_action_table[i].action != NULL))
        {
            s_key_action_table[i].action(event);
            break;
        }
    }
}

/*****************************************************************************
 * @brief:  Key scan task (AppKey).
 *          Waits for key events from key_service in a loop and dispatches them
 *          to the corresponding action functions through s_key_action_table.
 *          Prefer extending the mapping table when adding new key behaviors.
 * @para:   arg  Task argument; unused and passed as NULL
 * @return: None; infinite-loop task that never returns
 *****************************************************************************/
static void app_key_task(void *arg)
{
    key_event_t event;

    (void)arg;

    while (1)
    {
        /* Block until a key event is available. */
        if (key_service_get_event(&event, portMAX_DELAY) != RET_OK)
        {
            continue;
        }

        app_key_dispatch_event(&event);
    }
}

/*****************************************************************************
 * @brief:  Encoder scan task (AppEncoder).
 *          Waits for rotation events from encoder_service in a loop and
 *          adjusts the currently selected edit item according to the direction:
 *            - Clockwise rotation -> value +1
 *            - Counterclockwise rotation -> value -1
 *          The app_display edit-mode state determines which parameter is edited.
 * @para:   arg  Task argument; unused and passed as NULL
 * @return: None; infinite-loop task that never returns
 *****************************************************************************/
static void app_encoder_task(void *arg)
{
    encoder_event_t event;

    (void)arg;

    while (1)
    {
        /* Block until an encoder rotation event is available. */
        if (encoder_service_get_event(&event, portMAX_DELAY) != RET_OK)
        {
            continue;
        }

        if (event.rotate == ENCODER_ROTATE_CW)
        {
            /* Clockwise: increase the current edit-item value. */
            (void)app_display_adjust(APP_DISPLAY_ENCODER_VALUE_ADD);
        }
        else if (event.rotate == ENCODER_ROTATE_CCW)
        {
            /* Counterclockwise: decrease the current edit-item value. */
            (void)app_display_adjust(APP_DISPLAY_ENCODER_VALUE_REDUCE);
        }
    }
}

/*****************************************************************************
 * @brief:  Remote receiver task (AppRemote).
 *          Waits for remote events from remote_service in a loop and handles
 *          the following scenarios:
 *            1. Pairing mode: writes the received remote ID code to parameters,
 *               persists it, and completes pairing.
 *            2. Normal run mode:
 *               - Verifies that the remote ID code matches the stored one.
 *               - Dispatches by command code: unlock / mode toggle / lock.
 *          Remote data format (event.data[]):
 *            data[0..1] = remote ID code for pairing identification
 *            data[2] = command code (APP_REMOTE_UNLOCK / SWITCH_MODE / LOCK)
 * @para:   arg  Task argument; unused and passed as NULL
 * @return: None; infinite-loop task that never returns
 *****************************************************************************/
static void app_remote_task(void *arg)
{
    remote_event_t event;
    app_servo_params_t params;
    uint16_t remote_key_code;       // Two-byte fixed code for each remote controller.

    (void)arg;

    while (1)
    {
        /* Block until a remote event is available. */
        if (remote_service_get_event(&event, portMAX_DELAY) != RET_OK)
        {
            continue;
        }

        remote_key_code = ((uint16_t)event.data[0] << 8) | (uint16_t)event.data[1];

        /* ----- Scenario 1: pairing mode ----- */
        if (app_hmi_is_pairing() != 0U)
        {
            if (app_state_get_servo_params(&params) == RET_OK)
            {
                /* Write the received remote ID code into system parameters. */
                params.remote_key_code = remote_key_code;
                (void)app_hmi_save_params(&params, 0U);
                (void)app_hw_bluetooth_write_text("遥控器对码成功\r\n");
            }
            /* Exit pairing mode regardless of whether pairing succeeded. */
            app_hmi_set_pairing(0U);
            continue;
        }

        /* ----- Scenario 2: normal run mode - identity verification ----- */
        /*
         * Three checks ensure safe operation:
         *   1. Parameters were read successfully.
         *   2. The stored remote ID code is nonzero, meaning pairing is complete.
         *   3. The received ID code matches the stored one, preventing accidental
         *      triggering by nearby remote controllers.
         */
        if ((app_state_get_servo_params(&params) != RET_OK) || (params.remote_key_code == 0U) || (params.remote_key_code != remote_key_code))
        {
            continue;
        }

        /* ----- Identity verified; dispatch according to the command code. ----- */
        switch (event.data[2])
        {
            case APP_REMOTE_UNLOCK:
                /* Unlock command: exit locked state with power-off return. */
                (void)app_control_post_command(APP_CONTROL_CMD_EXIT_LOCK_BY_REMOTE);
                break;

            case APP_REMOTE_SWITCH_MODE:
                /* Mode-toggle command: folded <-> unfolded. */
                (void)app_control_post_command(APP_CONTROL_CMD_TOGGLE_SERVO);
                break;

            case APP_REMOTE_LOCK:
                /* Lock command: enter locked state. */
                (void)app_control_post_command(APP_CONTROL_CMD_ENTER_LOCK);
                break;

            default:
                /* Unknown command code; ignore it. */
                break;
        }
    }
}

/* Bluetooth parameter command mapping table: command character -> params field offset. */
static const app_bt_param_command_t s_bt_param_commands[] =
{
    {
        .command = 'b',
        .min_value = APP_PARAM_ANGLE_INIT_MIN,
        .max_value = APP_PARAM_ANGLE_INIT_MAX,
        .field_offset = offsetof(app_servo_params_t, angle_init),
        .update_key_timing = 0U
    },
    {
        .command = 'c',
        .min_value = APP_PARAM_ANGLE_FOLD_MIN,
        .max_value = APP_PARAM_ANGLE_FOLD_MAX,
        .field_offset = offsetof(app_servo_params_t, angle_fold),
        .update_key_timing = 0U
    },
    {
        .command = 'd',
        .min_value = 1U,
        .max_value = APP_PARAM_SERVO_SPEED_MAX,
        .field_offset = offsetof(app_servo_params_t, servo_speed),
        .update_key_timing = 0U
    },
    {
        .command = 'e',
        .min_value = 0U,
        .max_value = 1U,
        .field_offset = offsetof(app_servo_params_t, power_off_return),
        .update_key_timing = 0U
    },
    {
        .command = 'f',
        .min_value = APP_PARAM_LOCK_SEC_MIN,
        .max_value = APP_PARAM_LOCK_SEC_MAX,
        .field_offset = offsetof(app_servo_params_t, enter_lock_sec),
        .update_key_timing = 1U
    },
    {
        .command = 'g',
        .min_value = APP_PARAM_UNLOCK_SEC_MIN,
        .max_value = APP_PARAM_UNLOCK_SEC_MAX,
        .field_offset = offsetof(app_servo_params_t, enter_unlock_sec),
        .update_key_timing = 1U
    },
    {
        .command = 'n',
        .min_value = 0U,
        .max_value = 1U,
        .field_offset = offsetof(app_servo_params_t, memory_function_enable),
        .update_key_timing = 0U
    }
};

#define BT_PARAM_COMMANDS_SIZE sizeof(s_bt_param_commands) / sizeof(s_bt_param_commands[0])


static const app_bt_param_command_t *app_bt_find_param_command(uint8_t command)
{
    uint32_t i;

    for (i = 0U; i < BT_PARAM_COMMANDS_SIZE; i++)
    {
        if (s_bt_param_commands[i].command == command)
        {
            return &s_bt_param_commands[i];
        }
    }

    return NULL;
}

/*****************************************************************************
 * @brief:  Bluetooth command dispatch for parameter-setting commands.
 *          Parses the single-character command from a Bluetooth data frame,
 *          calls app_hmi_parse_u8_param() to extract the parameter value, writes
 *          the parsed value into the parameter structure, and finally persists
 *          it through app_hmi_save_params().
 *          Command mapping table (command character -> params field):
 *            'b' -> angle_init       Initial angle
 *            'c' -> angle_fold       Folded angle
 *            'd' -> servo_speed      Servo speed
 *            'e' -> power_off_return Power-off return
 *            'f' -> enter_lock_sec   Lock-entry delay
 *            'g' -> enter_unlock_sec Unlock-entry delay
 *            'n' -> memory_function_enable Memory function switch
 *          Commands 'f' and 'g' also synchronize key timing parameters when
 *          saved (update_key_timing=1).
 * @para:   command  Bluetooth command character (buf[0])
 * @para:   buf      Complete data frame buffer
 * @para:   len      Valid buffer data length
 * @return: None
 *****************************************************************************/
static void app_bt_handle_param_command(uint8_t command, const uint8_t *buf, uint32_t len)
{
    const app_bt_param_command_t *command_item;
    app_servo_params_t params;
    uint8_t value;
    int ret;

    command_item = app_bt_find_param_command(command);
    if (command_item == NULL)
    {
        return;
    }

    /* Fetch the current parameter snapshot first, using a read-modify-write flow. */
    if (app_state_get_servo_params(&params) != RET_OK)
    {
        return;
    }

    ret = app_hmi_parse_u8_param(buf, len, command_item->min_value, command_item->max_value, &value);
    if (RET_IS_OK(ret))
    {
        *((uint8_t *)((uint8_t *)&params + command_item->field_offset)) = value;
        (void)app_hmi_save_params(&params, command_item->update_key_timing);
    }
}

/*****************************************************************************
 * @brief:  Bluetooth UART receive/transmit task (AppBT).
 *          This is the most complex input-processing task in the system and is
 *          responsible for:
 *            1. Reading ASCII command frames from Bluetooth UART.
 *            2. Dispatching to different handling logic based on the first byte.
 *            3. Delegating parameter commands to app_bt_handle_param_command().
 *          Complete command character set; see the protocol table at the top
 *          of this file for details:
 *            'a' -> Start automatic calibration
 *            'b'/'c'/'d'/'e'/'f'/'g'/'n' -> Parameter-setting commands
 *            'h' -> Report all parameters
 *            'i' -> Toggle servo state
 *            'j'/'k' -> Set unlock mode
 *            'l'/'m' -> Lock/unlock control
 *          The task uses the larger APP_BT_TASK_STACK (768 words) to accommodate
 *          stack buffers that may be used during vsnprintf formatting.
 * @para:   arg  Task argument; unused and passed as NULL
 * @return: None; infinite-loop task that never returns
 *****************************************************************************/
static void app_bt_task(void *arg)
{
    uint8_t rx_buf[APP_BT_RX_BUF_LEN];  /* Receive buffer allocated on the stack. */
    int len;
    app_servo_params_t params;

    (void)arg;

    while (1)
    {
        /* Clear the buffer before each receive to prevent stale data from affecting parsing. */
        memset(rx_buf, 0, sizeof(rx_buf));

        /* Block while reading Bluetooth UART data; wait indefinitely until data arrives. */
        len = app_hw_bluetooth_read(rx_buf, DEV_UART_WAIT_FOREVER);
        if (len <= 0)
        {
            continue;
        }

#if CONFIG_USE_OTA
        /* Keep UART ownership in one task so binary OTA frames cannot race
           with the legacy ASCII command consumer. */
        if (ota_service_is_protocol_frame(rx_buf, (uint32_t)len) != 0U)
        {
            ota_service_process_frame(rx_buf, (uint32_t)len);
            continue;
        }
#endif

        /* Dispatch according to the first-byte command character. */
        switch (rx_buf[0])
        {
            case 'a':   /* Start automatic calibration. */
            {
                (void)app_control_post_command(APP_CONTROL_CMD_START_AUTO_CALIBRATE);
            }
            break;

            case 'b':
            case 'c':
            case 'd':
            case 'e':
            case 'f':
            case 'g':
            case 'n':   /* Parameter-setting commands, all parsed by the parameter handler. */
            {
                app_bt_handle_param_command(rx_buf[0], rx_buf, (uint32_t)len);
            }
            break;

            case 'h':   /* Report all current parameters to the Bluetooth terminal. */
            {
                app_hmi_bt_report_params();
            }
            break;

            case 'i':   /* Toggle the servo folded/unfolded state. */
            {
                (void)app_control_post_command(APP_CONTROL_CMD_TOGGLE_SERVO);
            }
            break;

            case 'j':   /* Set unlock mode to all sources allowed: key, remote, and Bluetooth. */
            {
                if (app_state_get_servo_params(&params) == RET_OK)
                {
                    params.unlock_mode = (uint8_t)APP_UNLOCK_MODE_ALL;
                    (void)app_hmi_save_params(&params, 0U);
                }
            }
            break;

            case 'k':   /* Set unlock mode to Bluetooth only, the highest security level. */
            {
                if (app_state_get_servo_params(&params) == RET_OK)
                {
                    params.unlock_mode = (uint8_t)APP_UNLOCK_MODE_BLE_ONLY;
                    (void)app_hmi_save_params(&params, 0U);
                }
            }
            break;

            case 'l':   /* Enter the locked state. */
            {
                (void)app_control_post_command(APP_CONTROL_CMD_ENTER_LOCK);
            }
            break;

            case 'm':   /* Exit the locked state with power-off return. */
            {
                (void)app_control_post_command(APP_CONTROL_CMD_EXIT_LOCK_BY_BLE);
            }
            break;

            default:
                /* Unknown command character; ignore silently to avoid abnormal behavior from invalid data. */
                break;
        }
    }
}

/*===========================================================================
 * External interfaces
 *===========================================================================*/

/*****************************************************************************
 * @brief:  Initialize the HMI module.
 *          Performs the following steps in order:
 *            1. Idempotency check: return RET_OK immediately if initialized.
 *            2. Create mutex s_hmi_mutex to protect pairing state and other
 *               shared data.
 *            3. Create four FreeRTOS tasks:
 *               - AppKey      Key scan task (512-word stack, priority 2)
 *               - AppEncoder  Encoder scan task (512-word stack, priority 2)
 *               - AppRemote   Remote receiver task (512-word stack, priority 2)
 *               - AppBT       Bluetooth UART task (768-word stack, priority 3)
 *            4. Set s_hmi_inited = 1.
 *          Note: if any task creation fails, an error is returned and already
 *          created tasks are not rolled back.
 *          The caller should invoke this during early system startup, after
 *          modules such as app_control and app_display are initialized.
 * @para:   None
 * @return: RET_OK            Initialization succeeded
 *          RET_NO_RESOURCE    Mutex creation or any task creation failed due to insufficient memory or invalid priority
 *****************************************************************************/
int app_hmi_init(void)
{
    /* Idempotency check: return success immediately if already initialized. */
    if (s_hmi_inited != 0U)
    {
        return RET_OK;
    }

    /* Create the mutex, a FreeRTOS kernel object used for exclusive access to shared state. */
    s_hmi_mutex = xSemaphoreCreateMutex();
    if (s_hmi_mutex == NULL)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the key scan task. */
    if (xTaskCreate(app_key_task, "AppKey", APP_HMI_TASK_STACK, NULL, APP_HMI_TASK_PRIO, NULL) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the encoder scan task. */
    if (xTaskCreate(app_encoder_task, "AppEncoder", APP_HMI_TASK_STACK, NULL, APP_HMI_TASK_PRIO, NULL) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the remote receiver task. */
    if (xTaskCreate(app_remote_task, "AppRemote", APP_HMI_TASK_STACK, NULL, APP_HMI_TASK_PRIO, NULL) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Create the Bluetooth UART receive/transmit task with larger stack and higher priority. */
    if (xTaskCreate(app_bt_task, "AppBT", APP_BT_TASK_STACK, NULL, APP_BT_TASK_PRIO, NULL) != pdPASS)
    {
        return RET_NO_RESOURCE;
    }

    /* Mark initialization complete. */
    s_hmi_inited = 1U;

    return RET_OK;
}
