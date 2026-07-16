/*****************************************************************************
 * @file    app_state.c
 * @brief   系统全局状态管理模块实现
 *          系统的状态数据中心，承担以下职责：
 *
 *          1. 参数集中管理
 *             - 在模块内部维护一份唯一的参数副本（s_app_params）
 *             - 所有参数读写操作均通过本模块的接口完成，确保数据源头唯一
 *             - 避免各模块各自缓存参数导致的数据不一致
 *
 *          2. 运行时状态追踪
 *             - 维护系统运行时状态结构体（s_app_runtime）
 *             - 追踪工作模式、舵机姿态、角度、电压、堵转 ADC 等实时信息
 *             - 提供单字段粒度的更新接口，减少锁持有时间
 *
 *          3. 线程安全保护
 *             - 使用 FreeRTOS 互斥量（Mutex）保护所有状态读写操作
 *             - 采用阻塞等待（portMAX_DELAY），确保关键区域一定可进入
 *             - 控制、显示、HMI 模块从不同任务并发访问时保证数据一致性
 *
 *          4. 原子性保证
 *             - 关联字段（姿态+角度、电压+堵转ADC）通过同一次加锁内更新
 *             - 避免读出一半旧值一半新值的中间状态
 *
 *          设计原则：
 *            本模块仅负责"数据是什么"，不执行任何业务逻辑。
 *            舵机驱动、显示更新、存储写入等动作由各自模块根据本模块提供的数据执行。
 * @author  LXA
 * @version v1.0
 * @date    2026-06-16
 *****************************************************************************/

#include "app_state.h"

/* FreeRTOS 内核组件 */
#include "FreeRTOS.h"
#include "semphr.h"               /* 互斥量（Mutex） */

#include "return_code.h"          /* 统一返回码 */

/*===========================================================================
 * 模块级静态变量
 *===========================================================================*/

/** 状态访问互斥量句柄（FreeRTOS Mutex），保护 s_app_params 和 s_app_runtime 的并发访问 */
static SemaphoreHandle_t s_app_state_mutex;

/** 系统参数缓存（模块内部唯一副本），所有参数读写均通过此变量 */
static app_servo_params_t s_app_servo_params;

/** 系统运行时状态缓存（模块内部唯一副本），所有状态读写均通过此变量 */
static app_servo_runtime_state_t s_app_servo_runtime;

/** 状态模块初始化标志：0=未初始化，1=已初始化（实现幂等性） */
static uint8_t s_app_state_inited;

/*===========================================================================
 * 内部工具函数
 *===========================================================================*/

/*****************************************************************************
 * @brief:  填充运行时状态默认值
 *          将 s_app_servo_runtime 初始化为安全的默认状态：
 *            - 工作模式：NORMAL（正常模式）
 *            - 舵机姿态：INIT（初始/展开状态）
 *            - 显示模式：NORMAL（显示当前角度）
 *            - 存储来源：UNKNOWN（尚未加载参数）
 *            - 角度/电压/ADC 均清零
 *          调用时机：模块初始化时调用一次。
 *          运行状态只保存"当前是什么状态"，不在这里执行舵机、显示或存储动作。
 * @para:   无
 * @return: 无
 *****************************************************************************/
static void app_state_fill_default_runtime(void)
{
    s_app_servo_runtime.work_mode = APP_WORK_MODE_NORMAL;
    s_app_servo_runtime.servo_pose = APP_SERVO_POSE_INIT;
    s_app_servo_runtime.display_mode = APP_DISPLAY_MODE_NORMAL;
    s_app_servo_runtime.storage_source = APP_STORAGE_SOURCE_UNKNOWN;
    s_app_servo_runtime.current_angle = 0U;
    s_app_servo_runtime.display_timeout_active = 0U;
    s_app_servo_runtime.power_voltage_mv = 0U;
    s_app_servo_runtime.locked_rotor_raw = 0U;
}

/*****************************************************************************
 * @brief:  获取状态互斥量（加锁）
 *          阻塞等待直到成功获取互斥量。调用方在访问 s_app_params 或
 *          s_app_servo_runtime 前必须先加锁，操作完毕后调用 app_state_unlock 解锁。
 *          后续控制、HMI、显示会从不同任务访问状态，因此统一用互斥锁保护。
 * @para:   无
 * @return: RET_OK            加锁成功
 *          RET_NOT_INITED     模块未初始化（互斥量不存在）
 *          RET_TIMEOUT        获取互斥量超时（理论上不会发生，因使用 portMAX_DELAY）
 *****************************************************************************/
static int app_state_lock(void)
{
    if (s_app_state_inited == 0U)
    {
        return RET_NOT_INITED;
    }

    if (xSemaphoreTake(s_app_state_mutex, portMAX_DELAY) != pdTRUE)
    {
        return RET_TIMEOUT;
    }

    return RET_OK;
}

/*****************************************************************************
 * @brief:  释放状态互斥量（解锁）
 *          与 app_state_lock 配对使用。
 * @para:   无
 * @return: 无
 *****************************************************************************/
static void app_state_unlock(void)
{
    (void)xSemaphoreGive(s_app_state_mutex);
}

/*===========================================================================
 * 对外接口
 *===========================================================================*/

/*****************************************************************************
 * @brief:  状态管理模块初始化
 *          按顺序完成：
 *            1. 幂等性检查（s_app_state_inited 标志，防重入）
 *            2. 创建互斥量（保护参数和运行时状态的并发访问）
 *            3. 填充运行时状态默认值
 *            4. 标记 s_app_state_inited = 1
 *          应在 FreeRTOS 内核启动后、其他应用模块初始化前调用。
 * @para:   无
 * @return: RET_OK            初始化成功（包括已初始化后再次调用）
 *          RET_NO_MEMORY      互斥量创建失败（内存不足）
 *****************************************************************************/
int app_state_init(void)
{
    /* 幂等性保证：已初始化则直接返回 */
    if (s_app_state_inited != 0U)
    {
        return RET_OK;
    }

    /* 创建互斥量保护状态数据 */
    s_app_state_mutex = xSemaphoreCreateMutex();
    if (s_app_state_mutex == 0)
    {
        return RET_NO_MEMORY;
    }

    /* 填充运行时状态默认值 */
    app_state_fill_default_runtime();

    /* 标记初始化完成 */
    s_app_state_inited = 1U;

    return RET_OK;
}

/*****************************************************************************
 * @brief:  写入系统参数（整体替换）
 *          加锁后将参数结构体完整拷贝到内部缓存 s_app_params。
 *          采用值拷贝而非指针引用，调用方可在调用后立即释放参数结构体。
 * @para:   params  指向源参数结构体（const，函数内部拷贝）
 * @return: RET_OK            写入成功
 *          RET_INVALID_PARAM  params 为空指针
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_servo_params(const app_servo_params_t *params)
{
    int ret;

    /* 空指针保护 */
    if (params == 0)
    {
        return RET_INVALID_PARAM;
    }

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 整体值拷贝，避免外部指针引用 */
    s_app_servo_params = *params;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  读取系统参数（整体拷贝）
 *          加锁后将内部缓存的参数结构体完整拷贝到调用方缓冲区。
 * @para:   params  指向目标参数结构体（由调用方分配内存）
 * @return: RET_OK            读取成功
 *          RET_INVALID_PARAM  params 为空指针
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_get_servo_params(app_servo_params_t *params)
{
    int ret;

    /* 空指针保护 */
    if (params == 0)
    {
        return RET_INVALID_PARAM;
    }

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 整体值拷贝，调用方获得独立副本 */
    *params = s_app_servo_params;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  写入运行时状态（整体替换）
 *          加锁后将运行时状态结构体完整拷贝到内部缓存 s_app_runtime。
 *          通常用于初始化或批量同步场景，日常更新建议使用单字段接口。
 * @para:   runtime  指向源运行时状态结构体
 * @return: RET_OK            写入成功
 *          RET_INVALID_PARAM  runtime 为空指针
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_runtime(const app_servo_runtime_state_t *runtime)
{
    int ret;

    /* 空指针保护 */
    if (runtime == 0)
    {
        return RET_INVALID_PARAM;
    }

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 整体值拷贝 */
    s_app_servo_runtime = *runtime;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  读取运行时状态（整体拷贝）
 *          加锁后将内部缓存的运行时状态结构体完整拷贝到调用方缓冲区。
 *          调用方获得独立副本，后续读取字段无需再加锁。
 * @para:   runtime  指向目标运行时状态结构体（由调用方分配内存）
 * @return: RET_OK            读取成功
 *          RET_INVALID_PARAM  runtime 为空指针
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_get_runtime(app_servo_runtime_state_t *runtime)
{
    int ret;

    /* 空指针保护 */
    if (runtime == 0)
    {
        return RET_INVALID_PARAM;
    }

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 整体值拷贝，调用方获得独立副本 */
    *runtime = s_app_servo_runtime;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  设置工作模式（单字段更新）
 *          线程安全地更新运行时状态中的 work_mode 字段。
 *          相比整体替换（app_state_set_runtime），单字段更新粒度更细，
 *          仅锁定必要的时间窗口，减少其他任务的等待延迟。
 * @para:   mode  目标工作模式
 * @return: RET_OK            设置成功
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_work_mode(app_work_mode_t mode)
{
    int ret;

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 单字段原子更新 */
    s_app_servo_runtime.work_mode = mode;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  设置舵机姿态和当前角度（双字段原子更新）
 *          线程安全地同时更新 servo_pose 和 current_angle 两个关联字段。
 *          在同一临界区内完成两个字段的写入，保证调用方不会观察到
 *          "姿态已更新但角度尚未更新"的中间不一致状态。
 * @para:   pose          目标舵机姿态
 * @para:   target_angle  目标舵机角度（0~180°）
 * @return: RET_OK            设置成功
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_servo_pose(app_servo_pose_t pose, uint8_t current_angle)
{
    int ret;

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 双字段原子更新，保证姿态与角度一致性 */
    s_app_servo_runtime.servo_pose = pose;
    s_app_servo_runtime.current_angle = current_angle;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  设置显示模式（单字段更新）
 *          线程安全地更新运行时状态中的 display_mode 字段。
 * @para:   mode  目标显示模式
 * @return: RET_OK            设置成功
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_display_mode(app_display_mode_t mode)
{
    int ret;

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 单字段原子更新 */
    s_app_servo_runtime.display_mode = mode;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  设置参数存储来源（单字段更新）
 *          记录最近一次参数加载的来源，用于上电诊断和面板显示。
 *          调用时机：app_storage_load_params 完成时调用。
 * @para:   source  存储来源
 * @return: RET_OK            设置成功
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_storage_source(app_storage_source_t source)
{
    int ret;

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 单字段原子更新 */
    s_app_servo_runtime.storage_source = source;
    app_state_unlock();

    return RET_OK;
}

/*****************************************************************************
 * @brief:  设置电源电压和堵转 ADC 采样值（双字段原子更新）
 *          线程安全地同时更新 power_voltage_mv 和 locked_rotor_raw。
 *          两个字段由控制模块的不同检测任务更新，在同一临界区内写入
 *          保证调用方读取时获得配对一致的电压和 ADC 快照。
 * @para:   power_voltage_mv   主电源电压（mV）
 * @para:   locked_rotor_raw   堵转检测 ADC 原始值
 * @return: RET_OK            设置成功
 *          其他                加锁失败时返回对应错误码
 *****************************************************************************/
int app_state_set_voltage(uint32_t power_voltage_mv, uint32_t locked_rotor_raw)
{
    int ret;

    ret = app_state_lock();
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    /* 双字段原子更新，保证电压与 ADC 读数的快照一致性 */
    s_app_servo_runtime.power_voltage_mv = power_voltage_mv;
    s_app_servo_runtime.locked_rotor_raw = locked_rotor_raw;
    app_state_unlock();

    return RET_OK;
}
