#include "digital_tube_service.h"

#include "return_code.h"

#include "FreeRTOS.h"
#include "timers.h"

#define DIGITAL_TUBE_SERVICE_DEFAULT_REFRESH_MS 1U

static TimerHandle_t s_digital_tube_timer;
static uint8_t s_service_initialized;

/*****************************************************************************
@brief: 数码管刷新软件定时器回调
@para:timer FreeRTOS 传入的软件定时器句柄，当前未使用
@return: 无
@note: 回调运行在 FreeRTOS Timer Service Task 上下文中，只执行一次轻量扫描刷新。
*******************************************************************************/
static void digital_tube_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    digital_tube_service_poll();
}

/*****************************************************************************
@brief: 初始化数码管服务
@para:cfg 应用层注入的数码管硬件配置和刷新周期
@return: RET_OK 表示成功，其它返回值表示失败
@note: 该函数负责初始化底层组件并创建周期软件定时器；如果 auto_start 非 0，会立即启动刷新。
*******************************************************************************/
int digital_tube_service_init(const digital_tube_service_cfg_t *cfg)
{
    uint16_t refresh_period_ms;
    int ret;

    if (cfg == 0)
    {
        return RET_INVALID_PARAM;
    }

    refresh_period_ms = (cfg->refresh_period_ms == 0U) ?
                        DIGITAL_TUBE_SERVICE_DEFAULT_REFRESH_MS : cfg->refresh_period_ms;

    ret = digital_tube_init(&cfg->tube_cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    s_digital_tube_timer = xTimerCreate("DTube",
                                        pdMS_TO_TICKS(refresh_period_ms),
                                        pdTRUE,
                                        (void *)0,
                                        digital_tube_timer_callback);
    if (s_digital_tube_timer == NULL)
    {
        return RET_NO_RESOURCE;
    }

    s_service_initialized = 1U;

    if (cfg->auto_start != 0U)
    {
        return digital_tube_service_start();
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 启动数码管扫描刷新
@return: RET_OK 表示成功，其它返回值表示失败
@note: 启动前会先允许底层组件显示，随后启动 FreeRTOS 软件定时器。
*******************************************************************************/
int digital_tube_service_start(void)
{
    int ret;

    if ((s_service_initialized == 0U) || (s_digital_tube_timer == NULL))
    {
        return RET_NOT_INITED;
    }

    ret = digital_tube_set_enable(1U);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (xTimerStart(s_digital_tube_timer, 0U) != pdPASS)
    {
        return RET_FAIL;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 停止数码管扫描刷新并熄灭显示
@return: RET_OK 表示成功，其它返回值表示失败
@note: 停止时会先请求软件定时器停止，再调用底层组件关闭所有段选和位选。
*******************************************************************************/
int digital_tube_service_stop(void)
{
    if ((s_service_initialized == 0U) || (s_digital_tube_timer == NULL))
    {
        return RET_NOT_INITED;
    }

    (void)xTimerStop(s_digital_tube_timer, 0U);

    return digital_tube_set_enable(0U);
}

/*****************************************************************************
@brief: 打开或关闭数码管显示
@para:enable 非 0 表示打开，0 表示关闭并熄灭
@return: RET_OK 表示成功，其它返回值表示失败
@note: 这是面向业务层的开关接口，内部会转到 start/stop，避免业务层直接操作定时器。
*******************************************************************************/
int digital_tube_service_set_enable(uint8_t enable)
{
    if (s_service_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    if (enable == 0U)
    {
        return digital_tube_service_stop();
    }

    return digital_tube_service_start();
}

/*****************************************************************************
@brief: 清空显示缓冲区
@return: RET_OK 表示成功，其它返回值表示失败
@note: 该接口只清空显示内容，不改变刷新定时器的启动状态。
*******************************************************************************/
int digital_tube_service_clear(void)
{
    if (s_service_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    return digital_tube_clear();
}

/*****************************************************************************
@brief: 设置单个数码管位
@para:pos 位置索引，0 表示最左侧，3 表示最右侧
@para:num 数字 0-9
@para:dp_enable 是否点亮小数点
@return: RET_OK 表示成功，其它返回值表示失败
@note: 适合需要逐位控制的场景，例如某一位显示小数点或自定义组合显示。
*******************************************************************************/
int digital_tube_service_set_digit(uint8_t pos, uint8_t num, uint8_t dp_enable)
{
    if (s_service_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    return digital_tube_set_digit(pos, num, dp_enable);
}

/*****************************************************************************
@brief: 显示无符号整数
@para:number 显示范围 0-9999，超出时按 9999 显示
@return: RET_OK 表示成功，其它返回值表示失败
@note: 这是业务层最常用的接口，内部会把整数转换为四位显示缓冲区。
*******************************************************************************/
int digital_tube_service_show_number(uint16_t number)
{
    if (s_service_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    return digital_tube_show_number(number);
}

/*****************************************************************************
@brief: 扫描刷新入口，通常由软件定时器周期调用
@return: 无
@note: 应用层一般不需要直接调用；如果后续不用软件定时器，也可以由固定周期任务主动调用。
*******************************************************************************/
void digital_tube_service_poll(void)
{
    digital_tube_scan_once();
}
