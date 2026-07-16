/*****************************************************************************
 * @file    app_control.h
 * @brief   舵机控制模块头文件
 *          作为系统的核心控制中枢，负责接收来自 HMI 模块（按键、编码器、遥控器、蓝牙）
 *          的控制命令，协调舵机运动、锁定/解锁状态切换、自动校准、堵转保护、
 *          断电检测及低电压自动回位等功能。
 *          对外提供命令投递接口（app_control_post_command）和初始化接口。
 * @author  GD32 Servo Control Team
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#ifndef APP_CONTROL_H
#define APP_CONTROL_H

#include <stdint.h>

/*===========================================================================
 * 控制命令枚举
 *===========================================================================*/

/**
 * @brief 舵机控制命令集
 *
 * 系统中所有对舵机及工作模式的操作请求均通过此枚举定义，
 * 经由命令队列（FreeRTOS Queue）异步投递给控制任务统一处理。
 * 命令语义按来源通道区分：
 *   - 按键触发：TOGGLE、ENTER_LOCK、EXIT_LOCK_BY_KEY
 *   - 遥控/蓝牙触发：TOGGLE、ENTER_LOCK、EXIT_LOCK_POWER_OFF
 *   - 系统内部：SERVO_INIT、SERVO_FOLD、START_AUTO_CALIBRATE
 */
typedef enum
{
    /** 正常模式下折叠/回位切换
     *  当前为折叠态则回到初始角，当前为初始态则运动到折叠角。 */
    APP_CONTROL_CMD_TOGGLE_SERVO = 0,

    /** 请求舵机运动到初始角度（回位） */
    APP_CONTROL_CMD_SERVO_INIT,

    /** 请求舵机运动到折叠角度 */
    APP_CONTROL_CMD_SERVO_FOLD,

    /** 请求进入锁定模式
     *  锁定前会先回初始角再切换工作模式，同时 LED 闪烁指示。 */
    APP_CONTROL_CMD_ENTER_LOCK,

    /** 按键通道退出锁定
     *  主电源仍在时保持续电供电，适用于按键超长按解锁场景。 */
    APP_CONTROL_CMD_EXIT_LOCK_BY_KEY,

    /** 遥控器通道退出锁定
     *  解锁后主动断开保持供电（关闭电源自锁电路），系统完全断电。 */
    APP_CONTROL_CMD_EXIT_LOCK_BY_REMOTE,

    /** 蓝牙通道退出锁定
     *  用于 APP_UNLOCK_MODE_BLE_ONLY 模式下保留唯一解锁入口。 */
    APP_CONTROL_CMD_EXIT_LOCK_BY_BLE,

    /** 启动自动校准流程
     *  舵机从中位出发，分别向高/低角度方向扫描，
     *  通过堵转检测自动测定折叠角与初始角的机械限位。 */
    APP_CONTROL_CMD_START_AUTO_CALIBRATE,
} app_control_cmd_t;

/*===========================================================================
 * 对外接口
 *===========================================================================*/

/*****************************************************************************
 * @brief:  控制模块初始化
 *          按顺序完成：
 *            1. 幂等性检查（s_control_inited 标志）
 *            2. 读取持久化参数，应用按键时间阈值
 *            3. 开启电源保持（防止启动阶段意外断电）
 *            4. 根据记忆的姿态将舵机驱动到初始角或折叠角
 *            5. 创建命令队列（长度 APP_CONTROL_QUEUE_LEN）
 *            6. 创建主控制任务 AppControl 和堵转检测任务 RotorCheck
 *          应在 app_state / servo_service / led_service 等底层模块之后调用。
 * @para:   无
 * @return: RET_OK            初始化成功
 *          RET_NOT_INITED     依赖模块（app_state）尚未初始化
 *          RET_NO_RESOURCE    队列或任务创建失败（内存不足）
 *****************************************************************************/
int app_control_init(void);

/*****************************************************************************
 * @brief:  向控制任务异步投递一条命令
 *          非阻塞接口，将命令写入队列后立即返回。
 *          若队列已满（控制任务处理不及时），返回 RET_BUSY。
 *          调用方可根据返回值决定是否重试或丢弃。
 * @para:   cmd  控制命令（参见 app_control_cmd_t 枚举）
 * @return: RET_OK            命令投递成功
 *          RET_NOT_INITED     控制模块尚未初始化（队列不存在）
 *          RET_BUSY           命令队列已满，投递失败
 *****************************************************************************/
int app_control_post_command(app_control_cmd_t cmd);

#endif /* APP_CONTROL_H */
