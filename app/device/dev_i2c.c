#include "dev_i2c.h"

#include "device.h"
#include "return_code.h"

/*****************************************************************************
@brief: 按名称查找 I2C 设备
@para:name I2C 设备名称
@return: 找到时返回设备指针，未找到或类型不匹配时返回 0
*******************************************************************************/
static device_t *dev_i2c_find(const char *name)
{
    device_t *dev;

    if (name == 0)
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_I2C))
    {
        return 0;
    }

    return dev;
}

/*****************************************************************************
@brief: 判断寄存器地址宽度是否合法
@para:width 寄存器地址宽度，单位：字节
@return: 合法返回 1，不合法返回 0
*******************************************************************************/
static uint8_t dev_i2c_reg_width_is_valid(uint8_t width)
{
    return ((width == 0U) || (width == 1U) || (width == 2U)) ? 1U : 0U;
}

/*****************************************************************************
@brief: 配置指定 I2C 设备
@para:name I2C 设备名称
@para:cfg I2C 总线配置指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_config(const char *name, const i2c_config_t *cfg)
{
    device_t *dev;
    int ret;

    if ((cfg == 0) || (cfg->speed_hz == 0U) || (cfg->addr_mode > I2C_ADDR_10BIT))
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_i2c_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_I2C_CMD_CONFIG, (void *)cfg);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 执行一组 I2C 消息传输
@para:name I2C 设备名称
@para:msgs I2C 消息数组
@para:num 消息数量
@return: RET_OK 表示传输成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_transfer(const char *name, i2c_msg_t *msgs, uint32_t num)
{
    device_t *dev;
    i2c_transfer_t transfer;
    uint32_t i;
    int ret;

    if ((msgs == 0) || (num == 0U))
    {
        return RET_INVALID_PARAM;
    }

    for (i = 0U; i < num; i++)
    {
        /* 设备层统一拦截空缓冲区、非法地址宽度和空消息，避免驱动层重复处理。 */
        if ((msgs[i].slave_addr > 0x7FU) ||
            (dev_i2c_reg_width_is_valid(msgs[i].reg_addr_width) == 0U) ||
            ((msgs[i].tx_len > 0U) && (msgs[i].tx_buf == 0)) ||
            ((msgs[i].rx_len > 0U) && (msgs[i].rx_buf == 0)) ||
            ((msgs[i].tx_len == 0U) && (msgs[i].rx_len == 0U)))
        {
            return RET_INVALID_PARAM;
        }
    }

    dev = dev_i2c_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    transfer.msgs = msgs;
    transfer.num = num;
    ret = device_control(dev, DEV_I2C_CMD_TRANSFER, &transfer);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 使用 1 字节寄存器地址写入 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:data 待写入数据缓冲区
@para:len 写入长度，单位：字节
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_write(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                  const uint8_t *data, uint32_t len)
{
    return dev_i2c_mem_write(name, slave_addr, reg_addr, 1U, data, len,
                             DEV_I2C_DEFAULT_TIMEOUT_MS);
}

/*****************************************************************************
@brief: 使用 1 字节寄存器地址读取 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:data 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_read(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                 uint8_t *data, uint32_t len)
{
    return dev_i2c_mem_read(name, slave_addr, reg_addr, 1U, data, len,
                            DEV_I2C_DEFAULT_TIMEOUT_MS);
}

/*****************************************************************************
@brief: 按指定寄存器地址宽度写入 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:reg_addr_width 寄存器地址宽度：0、1 或 2 字节
@para:data 待写入数据缓冲区
@para:len 写入长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示写入成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_mem_write(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                      uint8_t reg_addr_width, const uint8_t *data, uint32_t len,
                      uint32_t timeout_ms)
{
    i2c_msg_t msg;

    if ((data == 0) || (len == 0U) || (slave_addr > 0x7FU) ||
        (dev_i2c_reg_width_is_valid(reg_addr_width) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    /* 组装成单条写消息，由统一传输入口下发给具体驱动。 */
    msg.slave_addr = slave_addr;
    msg.reg_addr = reg_addr;
    msg.reg_addr_width = reg_addr_width;
    msg.tx_buf = data;
    msg.rx_buf = 0;
    msg.tx_len = len;
    msg.rx_len = 0U;
    msg.timeout_ms = timeout_ms;

    return dev_i2c_transfer(name, &msg, 1U);
}

/*****************************************************************************
@brief: 按指定寄存器地址宽度读取 I2C 从机数据
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:reg_addr 从机寄存器地址
@para:reg_addr_width 寄存器地址宽度：0、1 或 2 字节
@para:data 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示读取成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_mem_read(const char *name, uint8_t slave_addr, uint16_t reg_addr,
                     uint8_t reg_addr_width, uint8_t *data, uint32_t len,
                     uint32_t timeout_ms)
{
    i2c_msg_t msg;

    if ((data == 0) || (len == 0U) || (slave_addr > 0x7FU) ||
        (dev_i2c_reg_width_is_valid(reg_addr_width) == 0U))
    {
        return RET_INVALID_PARAM;
    }

    /* 组装成单条读消息，驱动层会根据 reg_addr_width 决定是否先写寄存器地址。 */
    msg.slave_addr = slave_addr;
    msg.reg_addr = reg_addr;
    msg.reg_addr_width = reg_addr_width;
    msg.tx_buf = 0;
    msg.rx_buf = data;
    msg.tx_len = 0U;
    msg.rx_len = len;
    msg.timeout_ms = timeout_ms;

    return dev_i2c_transfer(name, &msg, 1U);
}

/*****************************************************************************
@brief: 扫描 I2C 总线上应答的从机地址
@para:name I2C 设备名称
@para:addr_buf 地址输出缓冲区
@para:max_num addr_buf 最多可保存的地址数量
@para:found_num 实际发现的从机数量输出指针
@return: RET_OK 表示扫描成功，RET_BUFFER_TOO_SMALL 表示缓冲区不足，其他返回值表示失败
*******************************************************************************/
int dev_i2c_scan(const char *name, uint8_t *addr_buf, uint32_t max_num,
                 uint32_t *found_num)
{
    device_t *dev;
    i2c_scan_t scan;
    int ret;

    if ((addr_buf == 0) || (max_num == 0U) || (found_num == 0))
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_i2c_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    scan.addr_buf = addr_buf;
    scan.max_num = max_num;
    scan.found_num = found_num;
    scan.timeout_ms = DEV_I2C_DEFAULT_TIMEOUT_MS;
    ret = device_control(dev, DEV_I2C_CMD_SCAN, &scan);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 尝试恢复 I2C 总线
@para:name I2C 设备名称
@return: RET_OK 表示恢复成功，其他返回值表示失败
*******************************************************************************/
int dev_i2c_bus_recover(const char *name)
{
    device_t *dev;
    int ret;

    dev = dev_i2c_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = device_control(dev, DEV_I2C_CMD_BUS_RECOVER, 0);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

/*****************************************************************************
@brief: 探测指定 I2C 从机地址是否应答
@para:name I2C 设备名称
@para:slave_addr 7 位 I2C 从机地址
@para:timeout_ms 超时时间，单位：ms
@return: RET_OK 表示从机应答，其他返回值表示未应答或失败
*******************************************************************************/
int dev_i2c_probe(const char *name, uint8_t slave_addr, uint32_t timeout_ms)
{
    device_t *dev;
    i2c_probe_t probe;
    int ret;

    if (slave_addr > 0x7FU)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_i2c_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    probe.slave_addr = slave_addr;
    probe.timeout_ms = timeout_ms;
    ret = device_control(dev, DEV_I2C_CMD_PROBE, &probe);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}
