#include "drv_i2c.h"

#include "device.h"
#include "return_code.h"

#include <string.h>

#define GD32_I2C_DEVICE_MAX             2U          /* 最多注册的 GD32 I2C 设备数量 */
#define GD32_I2C_MAX_SPEED_HZ           400000U     /* 当前驱动支持的最高 I2C 速率 */
#define GD32_I2C_WAIT_LOOP_PER_MS       7200U       /* 轮询等待中 1ms 对应的循环预算 */
#define GD32_I2C_RECOVER_PULSE_COUNT    9U          /* 总线恢复时最多输出的 SCL 脉冲数 */
#define GD32_I2C_RECOVER_DELAY_LOOP     300U        /* 总线恢复 GPIO 翻转间隔 */

/* GD32 I2C 设备运行时描述，保存硬件资源和当前配置。 */
typedef struct
{
    uint32_t i2c_periph;                /* GD32 I2C 外设基地址标识 */
    rcu_periph_enum i2c_rcu;            /* I2C 外设时钟 */
    rcu_periph_enum scl_gpio_rcu;       /* SCL GPIO 端口时钟 */
    uint32_t scl_gpio_periph;           /* SCL GPIO 端口 */
    uint32_t scl_gpio_pin;              /* SCL GPIO 引脚 */
    rcu_periph_enum sda_gpio_rcu;       /* SDA GPIO 端口时钟 */
    uint32_t sda_gpio_periph;           /* SDA GPIO 端口 */
    uint32_t sda_gpio_pin;              /* SDA GPIO 引脚 */
    i2c_config_t config;                /* 当前 I2C 总线配置 */
} gd32_i2c_dev_t;

/* I2C 注册槽，绑定通用设备对象和 GD32 私有数据。 */
typedef struct
{
    device_t dev;           /* 设备框架对象 */
    gd32_i2c_dev_t i2c;     /* GD32 I2C 私有数据 */
} gd32_i2c_slot_t;

static gd32_i2c_slot_t s_i2c_slots[GD32_I2C_DEVICE_MAX];    /* I2C 设备注册槽 */
static uint8_t s_i2c_count;                                 /* 当前已注册 I2C 设备数量 */

/*****************************************************************************
@brief: 将毫秒超时转换为轮询循环预算
@para:timeout_ms 超时时间，单位：ms
@return: 轮询循环预算，DEV_I2C_WAIT_FOREVER 表示永久等待
*******************************************************************************/
static uint32_t gd32_i2c_timeout_budget(uint32_t timeout_ms)
{
    if (timeout_ms == DEV_I2C_WAIT_FOREVER)
    {
        return DEV_I2C_WAIT_FOREVER;
    }

    if (timeout_ms == 0U)
    {
        return 1U;
    }

    if (timeout_ms > (UINT32_MAX / GD32_I2C_WAIT_LOOP_PER_MS))
    {
        return UINT32_MAX - 1U;
    }

    return timeout_ms * GD32_I2C_WAIT_LOOP_PER_MS;
}

/*****************************************************************************
@brief: 消耗一次轮询预算并判断是否超时
@para:budget 轮询预算指针
@return: RET_OK 表示仍可继续等待，RET_TIMEOUT 表示超时
*******************************************************************************/
static int gd32_i2c_wait_next(uint32_t *budget)
{
    if (*budget == DEV_I2C_WAIT_FOREVER)
    {
        return RET_OK;
    }

    if (*budget == 0U)
    {
        return RET_TIMEOUT;
    }

    (*budget)--;
    return RET_OK;
}

/*****************************************************************************
@brief: 总线恢复时使用的短延时
@return: 无
*******************************************************************************/
static void gd32_i2c_short_delay(void)
{
    volatile uint32_t i;

    for (i = 0U; i < GD32_I2C_RECOVER_DELAY_LOOP; i++)
    {
    }
}

/*****************************************************************************
@brief: 将 7 位 I2C 从机地址转换为硬件地址字节
@para:slave_addr 7 位 I2C 从机地址
@return: 左移后的 I2C 地址字节，最低位保留给读写方向
*******************************************************************************/
static uint32_t gd32_i2c_addr_byte(uint8_t slave_addr)
{
    return ((uint32_t)slave_addr << 1U) & 0xFEU;
}

/*****************************************************************************
@brief: 检查并清除 I2C 错误标志
@para:i2c_periph GD32 I2C 外设基地址标识
@return: RET_OK 表示无错误，其他返回值表示对应 I2C 错误
*******************************************************************************/
static int gd32_i2c_clear_and_check_error(uint32_t i2c_periph)
{
    if (i2c_flag_get(i2c_periph, I2C_FLAG_BERR) == SET)
    {
        i2c_flag_clear(i2c_periph, I2C_FLAG_BERR);
        return RET_IO_ERROR;
    }

    if (i2c_flag_get(i2c_periph, I2C_FLAG_LOSTARB) == SET)
    {
        i2c_flag_clear(i2c_periph, I2C_FLAG_LOSTARB);
        return RET_BUSY;
    }

    if (i2c_flag_get(i2c_periph, I2C_FLAG_AERR) == SET)
    {
        i2c_flag_clear(i2c_periph, I2C_FLAG_AERR);
        return RET_NOT_FOUND;
    }

    if (i2c_flag_get(i2c_periph, I2C_FLAG_OUERR) == SET)
    {
        i2c_flag_clear(i2c_periph, I2C_FLAG_OUERR);
        return RET_IO_ERROR;
    }

    if (i2c_flag_get(i2c_periph, I2C_FLAG_PECERR) == SET)
    {
        i2c_flag_clear(i2c_periph, I2C_FLAG_PECERR);
        return RET_IO_ERROR;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 等待指定 I2C 标志达到目标状态
@para:i2c_periph GD32 I2C 外设基地址标识
@para:flag 待等待的 I2C 标志
@para:status 目标标志状态
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示等待成功，其他返回值表示超时或 I2C 错误
*******************************************************************************/
static int gd32_i2c_wait_flag(uint32_t i2c_periph, i2c_flag_enum flag,
                              FlagStatus status, uint32_t timeout_ms)
{
    uint32_t budget = gd32_i2c_timeout_budget(timeout_ms);
    int ret;

    while (i2c_flag_get(i2c_periph, flag) != status)
    {
        /* 等待标志期间同步检查错误标志，避免总线异常时一直等到超时。 */
        ret = gd32_i2c_clear_and_check_error(i2c_periph);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = gd32_i2c_wait_next(&budget);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 等待 STOP 条件发送完成
@para:i2c_periph GD32 I2C 外设基地址标识
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示 STOP 完成，其他返回值表示超时
*******************************************************************************/
static int gd32_i2c_wait_stop_done(uint32_t i2c_periph, uint32_t timeout_ms)
{
    uint32_t budget = gd32_i2c_timeout_budget(timeout_ms);
    int ret;

    while ((I2C_CTL0(i2c_periph) & I2C_CTL0_STOP) != 0U)
    {
        ret = gd32_i2c_wait_next(&budget);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 等待地址发送完成并清除地址标志
@para:i2c_periph GD32 I2C 外设基地址标识
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示地址阶段完成，其他返回值表示超时或 I2C 错误
*******************************************************************************/
static int gd32_i2c_wait_addr(uint32_t i2c_periph, uint32_t timeout_ms)
{
    uint32_t budget = gd32_i2c_timeout_budget(timeout_ms);
    int ret;

    while (i2c_flag_get(i2c_periph, I2C_FLAG_ADDSEND) == RESET)
    {
        ret = gd32_i2c_clear_and_check_error(i2c_periph);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        ret = gd32_i2c_wait_next(&budget);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    /* GD32 要求地址发送完成后清除 ADDSEND，后续收发状态才会继续推进。 */
    i2c_flag_clear(i2c_periph, I2C_FLAG_ADDSEND);

    return RET_OK;
}

/*****************************************************************************
@brief: 等待 I2C 总线空闲
@para:i2c_periph GD32 I2C 外设基地址标识
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示总线空闲，其他返回值表示超时或 I2C 错误
*******************************************************************************/
static int gd32_i2c_wait_idle(uint32_t i2c_periph, uint32_t timeout_ms)
{
    return gd32_i2c_wait_flag(i2c_periph, I2C_FLAG_I2CBSY, RESET, timeout_ms);
}

/*****************************************************************************
@brief: 连续写出一段 I2C 数据
@para:i2c_periph GD32 I2C 外设基地址标识
@para:buf 待发送数据缓冲区
@para:len 发送长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_write_bytes(uint32_t i2c_periph, const uint8_t *buf,
                                uint32_t len, uint32_t timeout_ms)
{
    uint32_t i;
    int ret;

    for (i = 0U; i < len; i++)
    {
        ret = gd32_i2c_wait_flag(i2c_periph, I2C_FLAG_TBE, SET, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        i2c_data_transmit(i2c_periph, buf[i]);

        ret = gd32_i2c_wait_flag(i2c_periph, I2C_FLAG_BTC, SET, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 发送 I2C 起始条件并进入写方向
@para:i2c GD32 I2C 设备运行时描述指针
@para:slave_addr 7 位 I2C 从机地址
@para:timeout_ms 超时时间，单位：ms
@para:wait_idle 非 0 表示起始前等待总线空闲
@return: RET_OK 表示写方向地址阶段完成，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_start_write(gd32_i2c_dev_t *i2c, uint8_t slave_addr,
                                uint32_t timeout_ms, uint8_t wait_idle)
{
    int ret;

    if (wait_idle != 0U)
    {
        ret = gd32_i2c_wait_idle(i2c->i2c_periph, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    /* 先发送 START，再发送写方向地址；失败时补发 STOP 释放总线。 */
    i2c_start_on_bus(i2c->i2c_periph);
    ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_SBSEND, SET, timeout_ms);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    i2c_master_addressing(i2c->i2c_periph, gd32_i2c_addr_byte(slave_addr), I2C_TRANSMITTER);
    ret = gd32_i2c_wait_addr(i2c->i2c_periph, timeout_ms);
    if (RET_IS_ERR(ret))
    {
        i2c_stop_on_bus(i2c->i2c_periph);
        (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
        return ret;
    }

    return gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_TBE, SET, timeout_ms);
}

/*****************************************************************************
@brief: 执行 I2C 原始写操作
@para:i2c GD32 I2C 设备运行时描述指针
@para:slave_addr 7 位 I2C 从机地址
@para:prefix 写数据前缀缓冲区，通常为寄存器地址
@para:prefix_len 前缀长度，单位：字节
@para:data 待写入数据缓冲区
@para:data_len 写入数据长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@para:send_stop 非 0 表示写完后发送 STOP
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_write_raw(gd32_i2c_dev_t *i2c, uint8_t slave_addr,
                              const uint8_t *prefix, uint32_t prefix_len,
                              const uint8_t *data, uint32_t data_len,
                              uint32_t timeout_ms, uint8_t send_stop)
{
    int ret;

    ret = gd32_i2c_start_write(i2c, slave_addr, timeout_ms, 1U);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    if (prefix_len > 0U)
    {
        /* 前缀用于先写寄存器地址，方便后续普通写或重复起始读。 */
        ret = gd32_i2c_write_bytes(i2c->i2c_periph, prefix, prefix_len, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            i2c_stop_on_bus(i2c->i2c_periph);
            (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
            return ret;
        }
    }

    if (data_len > 0U)
    {
        ret = gd32_i2c_write_bytes(i2c->i2c_periph, data, data_len, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            i2c_stop_on_bus(i2c->i2c_periph);
            (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
            return ret;
        }
    }

    ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_BTC, SET, timeout_ms);
    if (RET_IS_ERR(ret))
    {
        i2c_stop_on_bus(i2c->i2c_periph);
        (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
        return ret;
    }

    if (send_stop != 0U)
    {
        i2c_stop_on_bus(i2c->i2c_periph);
        ret = gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 执行 I2C 原始读操作
@para:i2c GD32 I2C 设备运行时描述指针
@para:slave_addr 7 位 I2C 从机地址
@para:data 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@para:wait_idle 非 0 表示起始前等待总线空闲
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_read_raw(gd32_i2c_dev_t *i2c, uint8_t slave_addr,
                             uint8_t *data, uint32_t len,
                             uint32_t timeout_ms, uint8_t wait_idle)
{
    uint32_t rx_len = len;
    int ret;

    if (wait_idle != 0U)
    {
        ret = gd32_i2c_wait_idle(i2c->i2c_periph, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    if (rx_len == 2U)
    {
        /* 2 字节接收需要提前配置 ACK 位置，确保最后两个字节按硬件手册时序 NACK。 */
        i2c_ackpos_config(i2c->i2c_periph, I2C_ACKPOS_NEXT);
    }

    i2c_start_on_bus(i2c->i2c_periph);
    ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_SBSEND, SET, timeout_ms);
    if (RET_IS_ERR(ret))
    {
        goto read_exit;
    }

    i2c_master_addressing(i2c->i2c_periph, gd32_i2c_addr_byte(slave_addr), I2C_RECEIVER);

    if (rx_len < 3U)
    {
        /* 1/2 字节读在地址阶段后就需要关闭 ACK，避免多接收字节。 */
        i2c_ack_config(i2c->i2c_periph, I2C_ACK_DISABLE);
    }

    ret = gd32_i2c_wait_addr(i2c->i2c_periph, timeout_ms);
    if (RET_IS_ERR(ret))
    {
        i2c_stop_on_bus(i2c->i2c_periph);
        (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
        goto read_exit;
    }

    if (rx_len == 1U)
    {
        /* 单字节读清除地址标志后立即发送 STOP。 */
        i2c_stop_on_bus(i2c->i2c_periph);
    }

    while (rx_len > 0U)
    {
        if (rx_len == 3U)
        {
            /* 还剩 3 字节时关闭 ACK，为最后两个字节的 NACK/STOP 时序做准备。 */
            ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_BTC, SET, timeout_ms);
            if (RET_IS_ERR(ret))
            {
                goto read_error_stop;
            }
            i2c_ack_config(i2c->i2c_periph, I2C_ACK_DISABLE);
        }

        if (rx_len == 2U)
        {
            /* 还剩 2 字节时发送 STOP，之后继续读出硬件接收缓冲中的数据。 */
            ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_BTC, SET, timeout_ms);
            if (RET_IS_ERR(ret))
            {
                goto read_error_stop;
            }
            i2c_stop_on_bus(i2c->i2c_periph);
        }

        ret = gd32_i2c_wait_flag(i2c->i2c_periph, I2C_FLAG_RBNE, SET, timeout_ms);
        if (RET_IS_ERR(ret))
        {
            goto read_error_stop;
        }

        *data = i2c_data_receive(i2c->i2c_periph);
        data++;
        rx_len--;
    }

    ret = gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
    goto read_exit;

read_error_stop:
    i2c_stop_on_bus(i2c->i2c_periph);
    (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);

read_exit:
    /* 恢复默认 ACK 配置，避免影响下一次传输。 */
    i2c_ack_config(i2c->i2c_periph, I2C_ACK_ENABLE);
    i2c_ackpos_config(i2c->i2c_periph, I2C_ACKPOS_CURRENT);

    return ret;
}

/*****************************************************************************
@brief: 将寄存器地址转换为 I2C 发送字节序列
@para:reg_addr 寄存器地址
@para:reg_width 寄存器地址宽度，单位：字节
@para:reg_buf 地址输出缓冲区
@return: 输出到 reg_buf 的地址字节数量
*******************************************************************************/
static uint32_t gd32_i2c_reg_to_buf(uint16_t reg_addr, uint8_t reg_width,
                                    uint8_t reg_buf[2])
{
    if (reg_width == 0U)
    {
        return 0U;
    }

    if (reg_width == 1U)
    {
        reg_buf[0] = (uint8_t)reg_addr;
        return 1U;
    }

    reg_buf[0] = (uint8_t)(reg_addr >> 8);
    reg_buf[1] = (uint8_t)reg_addr;
    return 2U;
}

/*****************************************************************************
@brief: 执行单条 I2C 消息传输
@para:i2c GD32 I2C 设备运行时描述指针
@para:msg I2C 消息描述指针
@return: RET_OK 表示传输成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_message_transfer(gd32_i2c_dev_t *i2c, const i2c_msg_t *msg)
{
    uint8_t reg_buf[2];
    uint32_t reg_len;
    uint32_t timeout_ms;
    int ret;

    timeout_ms = (msg->timeout_ms == 0U) ? DEV_I2C_DEFAULT_TIMEOUT_MS : msg->timeout_ms;
    reg_len = gd32_i2c_reg_to_buf(msg->reg_addr, msg->reg_addr_width, reg_buf);

    if (msg->rx_len == 0U)
    {
        /* 只有发送长度时执行普通写，寄存器地址作为前缀一起发送。 */
        return gd32_i2c_write_raw(i2c, msg->slave_addr, reg_buf, reg_len,
                                  msg->tx_buf, msg->tx_len, timeout_ms, 1U);
    }

    if ((reg_len == 0U) && (msg->tx_len == 0U))
    {
        /* 没有寄存器地址和写前缀时，直接进入读方向。 */
        return gd32_i2c_read_raw(i2c, msg->slave_addr, msg->rx_buf,
                                 msg->rx_len, timeout_ms, 1U);
    }

    /* 先写寄存器地址/前缀但不发 STOP，再用重复起始进入读方向。 */
    ret = gd32_i2c_write_raw(i2c, msg->slave_addr, reg_buf, reg_len,
                             msg->tx_buf, msg->tx_len, timeout_ms, 0U);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return gd32_i2c_read_raw(i2c, msg->slave_addr, msg->rx_buf,
                             msg->rx_len, timeout_ms, 0U);
}

/*****************************************************************************
@brief: 应用 I2C 硬件配置
@para:i2c GD32 I2C 设备运行时描述指针
@para:cfg I2C 总线配置指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_apply_config(gd32_i2c_dev_t *i2c, const i2c_config_t *cfg)
{
    if ((i2c == 0) || (cfg == 0) || (cfg->speed_hz == 0U) ||
        (cfg->speed_hz > GD32_I2C_MAX_SPEED_HZ))
    {
        return RET_INVALID_PARAM;
    }

    /* 当前 GD32 驱动仅实现 7 位主机模式，10 位地址由设备层保留接口。 */
    if (cfg->addr_mode != I2C_ADDR_7BIT)
    {
        return RET_NOT_SUPPORTED;
    }

    i2c_deinit(i2c->i2c_periph);
    i2c_clock_config(i2c->i2c_periph, cfg->speed_hz, I2C_DTCY_2);
    i2c_mode_addr_config(i2c->i2c_periph, I2C_I2CMODE_ENABLE,
                         I2C_ADDFORMAT_7BITS, cfg->own_addr);
    i2c_enable(i2c->i2c_periph);
    i2c_ack_config(i2c->i2c_periph, I2C_ACK_ENABLE);
    i2c_ackpos_config(i2c->i2c_periph, I2C_ACKPOS_CURRENT);
    i2c->config = *cfg;

    return RET_OK;
}

/*****************************************************************************
@brief: 初始化 GD32 I2C 硬件资源
@para:dev 设备层传入的 I2C 设备对象
@return: RET_OK 表示初始化成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_init(device_t *dev)
{
    gd32_i2c_dev_t *i2c;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    i2c = (gd32_i2c_dev_t *)dev->user_data;

    /* 使能 GPIO 和 I2C 外设时钟，并将 SCL/SDA 配置为复用开漏输出。 */
    rcu_periph_clock_enable(i2c->scl_gpio_rcu);
    rcu_periph_clock_enable(i2c->sda_gpio_rcu);
    rcu_periph_clock_enable(i2c->i2c_rcu);

    gpio_init(i2c->scl_gpio_periph, GPIO_MODE_AF_OD, GPIO_OSPEED_50MHZ, i2c->scl_gpio_pin);
    gpio_init(i2c->sda_gpio_periph, GPIO_MODE_AF_OD, GPIO_OSPEED_50MHZ, i2c->sda_gpio_pin);

    return gd32_i2c_apply_config(i2c, &i2c->config);
}

/*****************************************************************************
@brief: 探测指定 I2C 从机地址是否应答
@para:i2c GD32 I2C 设备运行时描述指针
@para:slave_addr 7 位 I2C 从机地址
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示从机应答，其他返回值表示未应答或失败
*******************************************************************************/
static int gd32_i2c_probe(gd32_i2c_dev_t *i2c, uint8_t slave_addr, uint32_t timeout_ms)
{
    int ret;

    ret = gd32_i2c_start_write(i2c, slave_addr, timeout_ms, 1U);
    if (RET_IS_OK(ret))
    {
        i2c_stop_on_bus(i2c->i2c_periph);
        (void)gd32_i2c_wait_stop_done(i2c->i2c_periph, timeout_ms);
    }

    return ret;
}

/*****************************************************************************
@brief: 扫描 I2C 总线上应答的从机地址
@para:i2c GD32 I2C 设备运行时描述指针
@para:scan I2C 扫描请求指针
@return: RET_OK 表示扫描成功，RET_BUFFER_TOO_SMALL 表示缓冲区不足，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_scan(gd32_i2c_dev_t *i2c, i2c_scan_t *scan)
{
    uint8_t addr;
    uint32_t found = 0U;
    uint32_t timeout_ms;
    int ret;

    if ((i2c == 0) || (scan == 0) || (scan->addr_buf == 0) ||
        (scan->max_num == 0U) || (scan->found_num == 0))
    {
        return RET_INVALID_PARAM;
    }

    timeout_ms = (scan->timeout_ms == 0U) ? DEV_I2C_DEFAULT_TIMEOUT_MS : scan->timeout_ms;

    /* 0x03~0x77 为常用 7 位 I2C 有效地址范围，避开保留地址。 */
    for (addr = 0x03U; addr <= 0x77U; addr++)
    {
        ret = gd32_i2c_probe(i2c, addr, timeout_ms);
        if (RET_IS_OK(ret))
        {
            if (found < scan->max_num)
            {
                scan->addr_buf[found] = addr;
            }
            found++;
        }
        else if ((ret != RET_NOT_FOUND) && (ret != RET_TIMEOUT))
        {
            *scan->found_num = found;
            return ret;
        }
    }

    *scan->found_num = found;

    return (found <= scan->max_num) ? RET_OK : RET_BUFFER_TOO_SMALL;
}

/*****************************************************************************
@brief: 通过 GPIO 时序尝试恢复 I2C 总线
@para:i2c GD32 I2C 设备运行时描述指针
@return: RET_OK 表示恢复成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_bus_recover(gd32_i2c_dev_t *i2c)
{
    uint8_t i;

    if (i2c == 0)
    {
        return RET_INVALID_PARAM;
    }

    /* 关闭 I2C 外设后临时接管 SCL/SDA，通过 SCL 脉冲释放被从机拉低的 SDA。 */
    i2c_disable(i2c->i2c_periph);

    gpio_init(i2c->scl_gpio_periph, GPIO_MODE_OUT_OD, GPIO_OSPEED_50MHZ, i2c->scl_gpio_pin);
    gpio_init(i2c->sda_gpio_periph, GPIO_MODE_OUT_OD, GPIO_OSPEED_50MHZ, i2c->sda_gpio_pin);
    gpio_bit_write(i2c->scl_gpio_periph, i2c->scl_gpio_pin, SET);
    gpio_bit_write(i2c->sda_gpio_periph, i2c->sda_gpio_pin, SET);
    gd32_i2c_short_delay();

    for (i = 0U; i < GD32_I2C_RECOVER_PULSE_COUNT; i++)
    {
        if (gpio_input_bit_get(i2c->sda_gpio_periph, i2c->sda_gpio_pin) == SET)
        {
            break;
        }

        gpio_bit_write(i2c->scl_gpio_periph, i2c->scl_gpio_pin, RESET);
        gd32_i2c_short_delay();
        gpio_bit_write(i2c->scl_gpio_periph, i2c->scl_gpio_pin, SET);
        gd32_i2c_short_delay();
    }

    /* 手动产生一个 STOP 条件，然后恢复 SCL/SDA 为 I2C 复用开漏功能。 */
    gpio_bit_write(i2c->sda_gpio_periph, i2c->sda_gpio_pin, RESET);
    gd32_i2c_short_delay();
    gpio_bit_write(i2c->scl_gpio_periph, i2c->scl_gpio_pin, SET);
    gd32_i2c_short_delay();
    gpio_bit_write(i2c->sda_gpio_periph, i2c->sda_gpio_pin, SET);
    gd32_i2c_short_delay();

    gpio_init(i2c->scl_gpio_periph, GPIO_MODE_AF_OD, GPIO_OSPEED_50MHZ, i2c->scl_gpio_pin);
    gpio_init(i2c->sda_gpio_periph, GPIO_MODE_AF_OD, GPIO_OSPEED_50MHZ, i2c->sda_gpio_pin);

    return gd32_i2c_apply_config(i2c, &i2c->config);
}

/*****************************************************************************
@brief: GD32 I2C 设备控制接口
@para:dev 设备层传入的 I2C 设备对象
@para:cmd I2C 控制命令
@para:arg 控制命令参数
@return: RET_OK 表示控制成功，其他返回值表示失败
*******************************************************************************/
static int gd32_i2c_control(device_t *dev, int cmd, void *arg)
{
    gd32_i2c_dev_t *i2c;
    i2c_transfer_t *transfer;
    i2c_probe_t *probe;
    uint32_t i;
    int ret;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    i2c = (gd32_i2c_dev_t *)dev->user_data;

    switch (cmd)
    {
    case DEV_I2C_CMD_CONFIG:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        return gd32_i2c_apply_config(i2c, (const i2c_config_t *)arg);

    case DEV_I2C_CMD_TRANSFER:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        transfer = (i2c_transfer_t *)arg;
        if ((transfer->msgs == 0) || (transfer->num == 0U))
        {
            return RET_INVALID_PARAM;
        }

        for (i = 0U; i < transfer->num; i++)
        {
            /* 多消息按顺序执行，任一消息失败立即返回错误。 */
            ret = gd32_i2c_message_transfer(i2c, &transfer->msgs[i]);
            if (RET_IS_ERR(ret))
            {
                return ret;
            }
        }
        return RET_OK;

    case DEV_I2C_CMD_SCAN:
        return gd32_i2c_scan(i2c, (i2c_scan_t *)arg);

    case DEV_I2C_CMD_BUS_RECOVER:
        return gd32_i2c_bus_recover(i2c);

    case DEV_I2C_CMD_PROBE:
        if (arg == 0)
        {
            return RET_INVALID_PARAM;
        }
        probe = (i2c_probe_t *)arg;
        if (probe->slave_addr > 0x7FU)
        {
            return RET_INVALID_PARAM;
        }
        return gd32_i2c_probe(i2c, probe->slave_addr,
                              (probe->timeout_ms == 0U) ? DEV_I2C_DEFAULT_TIMEOUT_MS : probe->timeout_ms);

    default:
        return RET_NOT_SUPPORTED;
    }
}

/* GD32 I2C 设备操作函数表。 */
static const device_ops_t s_gd32_i2c_ops =
{
    gd32_i2c_init,
    0,
    0,
    0,
    0,
    gd32_i2c_control,
};

/*****************************************************************************
@brief: 注册一个 GD32 I2C 设备
@para:cfg GD32 I2C 硬件资源配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int gd32_i2c_register(const gd32_i2c_cfg_t *cfg)
{
    gd32_i2c_slot_t *slot;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->default_config.speed_hz == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (s_i2c_count >= GD32_I2C_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    if (device_find(cfg->name) != 0)
    {
        return RET_ALREADY_EXISTS;
    }

    slot = &s_i2c_slots[s_i2c_count];
    memset(slot, 0, sizeof(*slot));

    /* 保存硬件资源与默认配置，实际硬件初始化由 device_init 触发。 */
    slot->i2c.i2c_periph = cfg->i2c_periph;
    slot->i2c.i2c_rcu = cfg->i2c_rcu;
    slot->i2c.scl_gpio_rcu = cfg->scl_gpio_rcu;
    slot->i2c.scl_gpio_periph = cfg->scl_gpio_periph;
    slot->i2c.scl_gpio_pin = cfg->scl_gpio_pin;
    slot->i2c.sda_gpio_rcu = cfg->sda_gpio_rcu;
    slot->i2c.sda_gpio_periph = cfg->sda_gpio_periph;
    slot->i2c.sda_gpio_pin = cfg->sda_gpio_pin;
    slot->i2c.config = cfg->default_config;

    slot->dev.name = cfg->name;
    slot->dev.type = DEVICE_CLASS_I2C;
    slot->dev.ops = &s_gd32_i2c_ops;
    slot->dev.user_data = &slot->i2c;
    slot->dev.flags = 0U;
    slot->dev.initialized = 0U;
    slot->dev.opened = 0U;
    slot->dev.ref_count = 0U;

    ret = device_register(&slot->dev);
    if (RET_IS_OK(ret))
    {
        s_i2c_count++;
    }

    return ret;
}
