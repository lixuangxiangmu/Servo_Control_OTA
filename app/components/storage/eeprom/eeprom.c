#include "eeprom.h"

#include "dev_i2c.h"
#include "return_code.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#define EEPROM_DEVICE_MAX               4U       /* 最多同时注册的 EEPROM 设备数量 */
#define EEPROM_DEFAULT_SLAVE_ADDR       0x50U    /* AT24 系列常用基础 7 位 I2C 地址 */
#define EEPROM_DEFAULT_WRITE_TIMEOUT_MS 20U      /* 默认写周期 ACK 轮询超时时间，单位 ms */
#define EEPROM_COMPARE_BUF_SIZE         32U      /* update 比较缓冲区大小 */

/* EEPROM 设备运行时描述，注册后保存到静态设备表中。 */
typedef struct
{
    const char *name;                   /* EEPROM 设备名 */
    const char *i2c_name;               /* 底层 I2C 设备名 */
    uint8_t slave_addr;                 /* 基础 7 位从机地址 */
    uint32_t total_size;                /* 总容量，单位：字节 */
    uint8_t page_size;                  /* 页大小，写入时不能跨页 */
    uint8_t addr_width;                 /* EEPROM 内部字地址宽度，单位：字节 */
    uint8_t block_bits;                 /* 从机地址低位中用于块选择的位数 */
    uint32_t write_cycle_timeout_ms;    /* 写周期 ACK 轮询超时时间，单位 ms */
} eeprom_dev_t;

static eeprom_dev_t s_eeprom_devices[EEPROM_DEVICE_MAX];    /* 已注册 EEPROM 设备表 */
static uint8_t s_eeprom_count;                              /* 当前已注册设备数量 */

/*****************************************************************************
@brief: 按名称查找已注册的 EEPROM 设备
@para:name EEPROM 设备名称
@return: 找到时返回 EEPROM 设备指针，未找到时返回 0
*******************************************************************************/
static eeprom_dev_t *eeprom_find(const char *name)
{
    uint8_t i;

    if ((name == 0) || (name[0] == '\0'))
    {
        return 0;
    }

    for (i = 0U; i < s_eeprom_count; i++)
    {
        if (strcmp(s_eeprom_devices[i].name, name) == 0)
        {
            return &s_eeprom_devices[i];
        }
    }

    return 0;
}

/*****************************************************************************
@brief: 根据配置中的型号填充 EEPROM 规格参数
@para:dev EEPROM 设备运行时描述指针
@para:cfg EEPROM 注册配置指针
@return: RET_OK 表示配置成功，其他返回值表示失败
*******************************************************************************/
static int eeprom_profile_apply(eeprom_dev_t *dev, const eeprom_cfg_t *cfg)
{
    if ((dev == 0) || (cfg == 0))
    {
        return RET_INVALID_PARAM;
    }

    switch (cfg->type)
    {
    case EEPROM_TYPE_AT24C08:
        dev->total_size = 1024U;
        dev->page_size = 16U;
        dev->addr_width = 1U;
        dev->block_bits = 2U;
        break;

    case EEPROM_TYPE_AT24C32:
        dev->total_size = 4096U;
        dev->page_size = 32U;
        dev->addr_width = 2U;
        dev->block_bits = 0U;
        break;

    case EEPROM_TYPE_CUSTOM:
        /* 自定义类型必须给出完整且可支持的参数，避免后续地址计算异常。 */
        if ((cfg->custom_total_size == 0U) ||
            (cfg->custom_page_size == 0U) ||
            ((cfg->custom_addr_width != 1U) && (cfg->custom_addr_width != 2U)) ||
            (cfg->custom_block_bits > 3U))
        {
            return RET_INVALID_PARAM;
        }

        dev->total_size = cfg->custom_total_size;
        dev->page_size = cfg->custom_page_size;
        dev->addr_width = cfg->custom_addr_width;
        dev->block_bits = cfg->custom_block_bits;
        break;

    default:
        return RET_INVALID_PARAM;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 返回两个 32 位无符号数中的较小值
@para:a 待比较数值 a
@para:b 待比较数值 b
@return: a 和 b 中的较小值
*******************************************************************************/
static uint32_t eeprom_min_u32(uint32_t a, uint32_t b)
{
    return (a < b) ? a : b;
}

/*****************************************************************************
@brief: 计算一个 I2C 从机地址块覆盖的字地址空间大小
@para:dev EEPROM 设备运行时描述指针
@return: 块大小，单位：字节；不使用块选择时返回 0
*******************************************************************************/
static uint32_t eeprom_block_size(const eeprom_dev_t *dev)
{
    if ((dev == 0) || (dev->block_bits == 0U))
    {
        return 0U;
    }

    return 1UL << (8U * dev->addr_width);
}

/*****************************************************************************
@brief: 根据线性地址计算实际访问时使用的 I2C 从机地址
@para:dev EEPROM 设备运行时描述指针
@para:addr EEPROM 线性地址
@return: 实际访问使用的 7 位 I2C 从机地址
*******************************************************************************/
static uint8_t eeprom_slave_addr_for_offset(const eeprom_dev_t *dev, uint32_t addr)
{
    uint32_t block_size;
    uint8_t block_mask;
    uint8_t block_index = 0U;

    block_size = eeprom_block_size(dev);
    if (block_size != 0U)
    {
        /* 小容量器件会把高位地址映射到从机地址低位，例如 AT24C08 的块选择位。 */
        block_mask = (uint8_t)((1U << dev->block_bits) - 1U);
        block_index = (uint8_t)((addr / block_size) & block_mask);
    }

    return (uint8_t)(dev->slave_addr | block_index);
}

/*****************************************************************************
@brief: 根据线性地址计算 EEPROM 内部字地址
@para:dev EEPROM 设备运行时描述指针
@para:addr EEPROM 线性地址
@return: EEPROM 内部字地址
*******************************************************************************/
static uint16_t eeprom_word_addr_for_offset(const eeprom_dev_t *dev, uint32_t addr)
{
    if (dev->addr_width == 1U)
    {
        return (uint16_t)(addr & 0xFFU);
    }

    return (uint16_t)(addr & 0xFFFFU);
}

/*****************************************************************************
@brief: 限制本次传输长度，确保不会跨越从机地址块边界
@para:dev EEPROM 设备运行时描述指针
@para:addr EEPROM 当前线性地址
@para:len 待传输长度，单位：字节
@return: 本次允许传输的长度，单位：字节
*******************************************************************************/
static uint32_t eeprom_limit_to_block(const eeprom_dev_t *dev, uint32_t addr, uint32_t len)
{
    uint32_t block_size;
    uint32_t remain_in_block;

    block_size = eeprom_block_size(dev);
    if (block_size == 0U)
    {
        return len;
    }

    remain_in_block = block_size - (addr % block_size);
    return eeprom_min_u32(len, remain_in_block);
}

/*****************************************************************************
@brief: 限制本次写入长度，确保不会跨越 EEPROM 页边界
@para:dev EEPROM 设备运行时描述指针
@para:addr EEPROM 当前线性地址
@para:len 待写入长度，单位：字节
@return: 本次允许写入的长度，单位：字节
*******************************************************************************/
static uint32_t eeprom_limit_to_page(const eeprom_dev_t *dev, uint32_t addr, uint32_t len)
{
    uint32_t remain_in_page;

    remain_in_page = (uint32_t)dev->page_size - (addr % (uint32_t)dev->page_size);

    return eeprom_min_u32(len, remain_in_page);
}

/*****************************************************************************
@brief: 校验 EEPROM 访问范围是否合法
@para:dev EEPROM 设备运行时描述指针
@para:addr EEPROM 起始地址
@para:len 访问长度，单位：字节
@return: RET_OK 表示范围合法，其他返回值表示参数无效
*******************************************************************************/
static int eeprom_validate_range(const eeprom_dev_t *dev, uint32_t addr, uint32_t len)
{
    if ((dev == 0) || (len == 0U) || (addr >= dev->total_size) ||
        (len > (dev->total_size - addr)))
    {
        return RET_INVALID_PARAM;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 写完成轮询间隔延时，调度器启动前使用忙等，启动后使用 RTOS 延时
@return: 无
*******************************************************************************/
static void eeprom_poll_delay(void)
{
    if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
    {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    else
    {
        volatile uint32_t i;

        for (i = 0U; i < 7200U; i++)
        {
        }
    }
}

/*****************************************************************************
@brief: 写入后轮询设备应答，等待 EEPROM 内部写周期完成
@para:dev EEPROM 设备运行时描述指针
@para:slave_addr 本次写入使用的 I2C 从机地址
@return: RET_OK 表示设备已就绪，RET_TIMEOUT 表示等待超时
*******************************************************************************/
static int eeprom_wait_ready(const eeprom_dev_t *dev, uint8_t slave_addr)
{
    uint32_t attempts;
    int ret = RET_TIMEOUT;

    attempts = dev->write_cycle_timeout_ms;
    if (attempts == 0U)
    {
        attempts = EEPROM_DEFAULT_WRITE_TIMEOUT_MS;
    }

    /* 第一次立即探测，后续每约 1ms 探测一次。+1 保证 5ms 配置会覆盖
     * t=0..5ms，而不是只探测到 t=4ms 就退出。 */
    attempts++;

    while (attempts > 0U)
    {
        /* 写周期内 EEPROM 通常不会应答 ACK，重新 ACK 表示可以继续访问。 */
        ret = dev_i2c_probe(dev->i2c_name, slave_addr, 1U);
        if (RET_IS_OK(ret))
        {
            return RET_OK;
        }

        eeprom_poll_delay();
        attempts--;
    }

    (void)ret;
    return RET_TIMEOUT;
}

/*****************************************************************************
@brief: 注册 EEPROM 设备并保存运行时参数
@para:cfg EEPROM 注册配置指针
@return: RET_OK 表示注册成功，其他返回值表示失败
*******************************************************************************/
int eeprom_register(const eeprom_cfg_t *cfg)
{
    eeprom_dev_t *dev;
    int ret;

    if ((cfg == 0) || (cfg->name == 0) || (cfg->i2c_name == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (s_eeprom_count >= EEPROM_DEVICE_MAX)
    {
        return RET_NO_RESOURCE;
    }

    if (eeprom_find(cfg->name) != 0)
    {
        return RET_ALREADY_EXISTS;
    }

    dev = &s_eeprom_devices[s_eeprom_count];
    memset(dev, 0, sizeof(*dev));

    dev->name = cfg->name;
    dev->i2c_name = cfg->i2c_name;
    dev->slave_addr = (cfg->slave_addr == 0U) ? EEPROM_DEFAULT_SLAVE_ADDR : cfg->slave_addr;
    dev->write_cycle_timeout_ms = cfg->write_cycle_timeout_ms;

    /* 仅接受 7 位 I2C 地址，避免把读写位误传入驱动层。 */
    if (dev->slave_addr > 0x7FU)
    {
        return RET_INVALID_PARAM;
    }

    ret = eeprom_profile_apply(dev, cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    s_eeprom_count++;

    return RET_OK;
}

/*****************************************************************************
@brief: 从 EEPROM 顺序读取数据，跨块时自动拆分为多次 I2C 读
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 读取数据存放缓冲区
@para:len 读取长度，单位：字节
@return: 成功返回实际读取长度，失败返回对应错误码
*******************************************************************************/
int eeprom_read(const char *name, uint32_t addr, uint8_t *buf, uint32_t len)
{
    eeprom_dev_t *dev;
    uint32_t offset = 0U;
    uint32_t chunk;
    uint8_t slave_addr;
    uint16_t word_addr;
    int ret;

    if (buf == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = eeprom_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = eeprom_validate_range(dev, addr, len);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    while (offset < len)
    {
        /* 读操作需要在从机地址块边界处分段，保证地址映射正确。 */
        chunk = eeprom_limit_to_block(dev, addr + offset, len - offset);
        slave_addr = eeprom_slave_addr_for_offset(dev, addr + offset);
        word_addr = eeprom_word_addr_for_offset(dev, addr + offset);

        ret = dev_i2c_mem_read(dev->i2c_name, slave_addr, word_addr,
                               dev->addr_width, &buf[offset], chunk,
                               DEV_I2C_DEFAULT_TIMEOUT_MS);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        offset += chunk;
    }

    return RET_SUCCESS_VALUE(len);
}

/*****************************************************************************
@brief: 向 EEPROM 顺序写入数据，内部按页边界和块边界自动拆分
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 待写入数据缓冲区
@para:len 写入长度，单位：字节
@return: 成功返回实际写入长度，失败返回对应错误码
*******************************************************************************/
int eeprom_write(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    eeprom_dev_t *dev;
    uint32_t offset = 0U;
    uint32_t chunk;
    uint8_t slave_addr;
    uint16_t word_addr;
    int ret;

    if (buf == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = eeprom_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = eeprom_validate_range(dev, addr, len);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    while (offset < len)
    {
        /* EEPROM 页写不能跨页；部分型号还不能跨从机地址块。 */
        chunk = eeprom_limit_to_page(dev, addr + offset, len - offset);
        chunk = eeprom_limit_to_block(dev, addr + offset, chunk);
        slave_addr = eeprom_slave_addr_for_offset(dev, addr + offset);
        word_addr = eeprom_word_addr_for_offset(dev, addr + offset);

        ret = dev_i2c_mem_write(dev->i2c_name, slave_addr, word_addr,
                                dev->addr_width, &buf[offset], chunk,
                                DEV_I2C_DEFAULT_TIMEOUT_MS);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        /* 每次页写后等待内部写周期完成，再继续下一段写入。 */
        ret = eeprom_wait_ready(dev, slave_addr);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        offset += chunk;
    }

    return RET_SUCCESS_VALUE(len);
}

/*****************************************************************************
@brief: 更新 EEPROM 数据，仅对内容变化的区域执行写入
@para:name EEPROM 设备名称
@para:addr EEPROM 起始地址
@para:buf 目标数据缓冲区
@para:len 更新长度，单位：字节
@return: 成功返回实际更新长度，失败返回对应错误码
*******************************************************************************/
int eeprom_update(const char *name, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint8_t read_buf[EEPROM_COMPARE_BUF_SIZE];
    uint32_t offset = 0U;
    uint32_t chunk;
    int ret;

    if (buf == 0)
    {
        return RET_INVALID_PARAM;
    }

    while (offset < len)
    {
        /* 分块比较，避免在栈上申请过大的临时缓冲区。 */
        chunk = eeprom_min_u32(len - offset, EEPROM_COMPARE_BUF_SIZE);
        ret = eeprom_read(name, addr + offset, read_buf, chunk);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        /* 数据不一致才写入，避免重复写相同内容。 */
        if (memcmp(read_buf, &buf[offset], chunk) != 0)
        {
            ret = eeprom_write(name, addr + offset, &buf[offset], chunk);
            if (RET_IS_ERR(ret))
            {
                return ret;
            }
        }

        offset += chunk;
    }

    return RET_SUCCESS_VALUE(len);
}

/*****************************************************************************
@brief: 查询已注册 EEPROM 的运行时参数
@para:name EEPROM 设备名称
@para:info EEPROM 信息输出指针
@return: RET_OK 表示查询成功，其他返回值表示失败
*******************************************************************************/
int eeprom_get_info(const char *name, eeprom_info_t *info)
{
    eeprom_dev_t *dev;

    if (info == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = eeprom_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    info->total_size = dev->total_size;
    info->page_size = dev->page_size;
    info->addr_width = dev->addr_width;
    info->block_bits = dev->block_bits;
    info->slave_addr = dev->slave_addr;

    return RET_OK;
}
