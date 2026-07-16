#include "drv_adc.h"

#include "device.h"
#include "dev_adc.h"
#include "return_code.h"

#include <string.h>

/* ADC 扫描组最大数量。
   当前板子只用 ADC0 一个扫描组，这里保留 2 个名额，方便以后扩展 ADC1/ADC2。 */
#define GD32_ADC_GROUP_MAX          2U

/* 单个扫描组最多支持的逻辑通道数量。
   GD32F10x regular sequence 能配置更多通道，但当前驱动先做轻量版本。 */
#define GD32_ADC_CHANNEL_MAX        8U

/* ADC 规则数据寄存器相对 ADC 外设基地址的偏移。
   DMA 从 ADC_RDATA 读取转换结果，和 GD32 标准库头文件里的 ADC_RDATA(adcx) 保持一致。 */
#define GD32_ADC_RDATA_OFFSET       0x4CU

/* ADC 扫描组运行时数据。
   一个 group 对应一个物理 ADC + 一个 DMA 通道 + 一组扫描通道。
   DMA 会把每一轮扫描结果依次搬到 samples[]，逻辑通道按 index 取对应位置。 */
typedef struct
{
    uint32_t adc_periph;                         /* ADC 外设基地址，例如 ADC0。 */
    rcu_periph_enum adc_rcu;                     /* ADC 外设时钟。 */
    uint32_t adc_clock_div;                      /* ADC 时钟分频配置。 */
    uint32_t dma_periph;                         /* DMA 外设基地址，例如 DMA0。 */
    rcu_periph_enum dma_rcu;                     /* DMA 外设时钟。 */
    dma_channel_enum dma_channel;                /* DMA 通道。 */
    const gd32_adc_channel_cfg_t *channels;      /* BSP 提供的通道配置数组。 */
    uint8_t channel_count;                       /* 扫描通道数量。 */
    uint8_t initialized;                         /* 扫描组是否已经完成硬件初始化。 */
    uint16_t samples[GD32_ADC_CHANNEL_MAX];      /* DMA 环形写入的 raw 采样缓存。 */
} gd32_adc_group_t;

/* 单个逻辑 ADC 设备的私有数据。
   group 指向所属扫描组，index 表示该设备读取 samples[] 的哪个位置。 */
typedef struct
{
    gd32_adc_group_t *group;     /* 该逻辑通道所属的 ADC 扫描组。 */
    uint8_t index;               /* 该逻辑通道在扫描序列和 samples[] 中的下标。 */
} gd32_adc_dev_t;

/* ADC 设备注册槽。
   每个 slot 绑定一个通用 device_t 和一个 ADC 逻辑通道私有数据。 */
typedef struct
{
    device_t dev;        /* 注册到 device 框架的通用设备对象。 */
    gd32_adc_dev_t adc;  /* GD32 ADC 逻辑通道私有数据。 */
} gd32_adc_slot_t;

/* 静态资源池：避免在嵌入式驱动中使用动态内存。 */
static gd32_adc_group_t s_adc_groups[GD32_ADC_GROUP_MAX];
static gd32_adc_slot_t s_adc_slots[GD32_ADC_GROUP_MAX * GD32_ADC_CHANNEL_MAX];
static uint8_t s_adc_group_count;
static uint8_t s_adc_slot_count;

/*****************************************************************************
@brief: ADC 使能后用于等待模拟电路稳定的短延时
@para:
@return:
@note:
  老工程在 ADC 使能后调用 DelayNus(50) 再校准。
  当前 RT 工程暂未提供统一微秒延时接口，因此这里使用短空循环等待。
*******************************************************************************/
static void gd32_adc_short_delay(void)
{
    volatile uint32_t i;

    for (i = 0U; i < 2000U; i++)
    {
    }
}

/*****************************************************************************
@brief: 初始化一个 GD32 ADC 扫描组
@para:group ADC 扫描组运行时对象
@return: RET_OK 表示初始化成功，其他返回值表示失败
@note:
  初始化顺序很重要：
  1. 打开 DMA/ADC/GPIO 时钟；
  2. 将输入 GPIO 配成模拟输入；
  3. 配置 DMA 从 ADC_RDATA 搬运到 samples[]；
  4. 配置 ADC 连续扫描模式；
  5. 使能 ADC、校准、软件触发，之后 DMA 会持续刷新 samples[]。
*******************************************************************************/
static int gd32_adc_group_init(gd32_adc_group_t *group)
{
    dma_parameter_struct dma_cfg;
    uint8_t i;

    if ((group == 0) || (group->channels == 0) || (group->channel_count == 0U) ||
        (group->channel_count > GD32_ADC_CHANNEL_MAX))
    {
        return RET_INVALID_PARAM;
    }

    if (group->initialized != 0U)
    {
        return RET_OK;
    }

    rcu_periph_clock_enable(group->dma_rcu);
    rcu_periph_clock_enable(group->adc_rcu);
    rcu_adc_clock_config(group->adc_clock_div);

    /* ADC 输入脚必须配置成模拟输入，避免数字输入缓冲影响模拟采样。 */
    for (i = 0U; i < group->channel_count; i++)
    {
        rcu_periph_clock_enable(group->channels[i].gpio_rcu);
        gpio_init(group->channels[i].gpio_periph,
                  GPIO_MODE_AIN,
                  GPIO_OSPEED_10MHZ,
                  group->channels[i].gpio_pin);
    }

    /* DMA 使用循环模式。
       ADC 每完成一个规则通道转换，就把 ADC_RDATA 的 16bit raw 值搬到 samples[]。
       扫描序列长度为 channel_count，因此 samples[0..channel_count-1] 对应一轮扫描结果。 */
    dma_deinit(group->dma_periph, group->dma_channel);
    dma_struct_para_init(&dma_cfg);
    dma_cfg.direction = DMA_PERIPHERAL_TO_MEMORY;
    dma_cfg.periph_addr = group->adc_periph + GD32_ADC_RDATA_OFFSET;
    dma_cfg.periph_inc = DMA_PERIPH_INCREASE_DISABLE;
    dma_cfg.periph_width = DMA_PERIPHERAL_WIDTH_16BIT;
    dma_cfg.memory_addr = (uint32_t)group->samples;
    dma_cfg.memory_inc = DMA_MEMORY_INCREASE_ENABLE;
    dma_cfg.memory_width = DMA_MEMORY_WIDTH_16BIT;
    dma_cfg.number = group->channel_count;
    dma_cfg.priority = DMA_PRIORITY_HIGH;
    dma_init(group->dma_periph, group->dma_channel, &dma_cfg);
    dma_circulation_enable(group->dma_periph, group->dma_channel);
    dma_channel_enable(group->dma_periph, group->dma_channel);

    /* ADC 配置为独立、连续、扫描、右对齐：
       - 独立模式：只使用当前 ADC，不与其他 ADC 做同步组合；
       - 连续模式：软件触发一次后持续转换；
       - 扫描模式：按 regular sequence 依次转换多个通道；
       - 右对齐：12bit 结果放在低 12bit，读取 uint16_t 即 raw 值。 */
    adc_deinit(group->adc_periph);
    adc_mode_config(ADC_MODE_FREE);
    adc_special_function_config(group->adc_periph, ADC_CONTINUOUS_MODE, ENABLE);
    adc_special_function_config(group->adc_periph, ADC_SCAN_MODE, ENABLE);
    adc_data_alignment_config(group->adc_periph, ADC_DATAALIGN_RIGHT);
    adc_channel_length_config(group->adc_periph, ADC_REGULAR_CHANNEL, group->channel_count);

    /* 配置规则通道扫描序列。
       rank 与 samples[] 下标保持一致：rank 0 的转换结果最终进入 samples[0]。 */
    for (i = 0U; i < group->channel_count; i++)
    {
        adc_regular_channel_config(group->adc_periph,
                                   i,
                                   group->channels[i].adc_channel,
                                   group->channels[i].sample_time);
    }

    /* 选择软件触发作为规则组触发源。
       外部触发配置保持 ENABLE，是 GD32 标准库使用软件触发时的常见配置方式。 */
    adc_external_trigger_source_config(group->adc_periph,
                                       ADC_REGULAR_CHANNEL,
                                       ADC0_1_2_EXTTRIG_REGULAR_NONE);
    adc_external_trigger_config(group->adc_periph, ADC_REGULAR_CHANNEL, ENABLE);

    /* ADC DMA 请求必须在软件触发前打开，否则转换结果不会自动搬到 samples[]。 */
    adc_dma_mode_enable(group->adc_periph);
    adc_enable(group->adc_periph);
    gd32_adc_short_delay();
    adc_calibration_enable(group->adc_periph);
    adc_software_trigger_enable(group->adc_periph, ADC_REGULAR_CHANNEL);

    group->initialized = 1U;

    return RET_OK;
}

/*****************************************************************************
@brief: device 框架调用的 ADC 初始化入口
@para:dev 通用设备对象
@return: RET_OK 表示成功，其他返回值表示失败
@note:
  dev->user_data 指向 gd32_adc_dev_t，再通过 gd32_adc_dev_t 找到所属 group。
  多个逻辑 ADC 设备可能属于同一个 group，group 内部用 initialized 防止重复初始化硬件。
*******************************************************************************/
static int gd32_adc_init(device_t *dev)
{
    gd32_adc_dev_t *adc;

    if ((dev == 0) || (dev->user_data == 0))
    {
        return RET_INVALID_PARAM;
    }

    adc = (gd32_adc_dev_t *)dev->user_data;

    return gd32_adc_group_init(adc->group);
}

/*****************************************************************************
@brief: device 框架调用的 ADC raw 读取入口
@para:dev 通用设备对象
@para:buf 输出缓冲区，至少需要 sizeof(uint16_t)
@para:len 输出缓冲区长度
@return: 成功时返回读取字节数，失败时返回错误码
@note:
  这里不主动启动一次转换，而是读取 DMA 环形缓存中的最新值。
  这样读取接口非常轻量，不会阻塞等待 ADC 转换完成。
*******************************************************************************/
static int gd32_adc_read(device_t *dev, uint8_t *buf, uint32_t len)
{
    gd32_adc_dev_t *adc;

    if ((dev == 0) || (dev->user_data == 0) || (buf == 0) || (len < sizeof(uint16_t)))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    adc = (gd32_adc_dev_t *)dev->user_data;
    if ((adc->group == 0) || (adc->index >= adc->group->channel_count))
    {
        return RET_INVALID_STATE;
    }

    /* 按逻辑通道下标读取对应 raw 值。
       例如 adc_power_raw 的 index 为 0，就读取 samples[0]。 */
    *(uint16_t *)buf = adc->group->samples[adc->index];

    return RET_SUCCESS_VALUE(sizeof(uint16_t));
}

/*****************************************************************************
@brief: device 框架调用的 ADC 控制入口
@para:dev 通用设备对象
@para:cmd 控制命令
@para:arg 控制参数
@return: RET_OK 表示成功，其他返回值表示失败
@note:
  当前第一版只要求按名称读取 raw 值，动态配置暂未实现。
*******************************************************************************/
static int gd32_adc_control(device_t *dev, int cmd, void *arg)
{
    (void)arg;

    if (dev == 0)
    {
        return RET_INVALID_PARAM;
    }

    switch (cmd)
    {
    case DEV_ADC_CMD_CONFIG:
        return RET_NOT_SUPPORTED;

    default:
        return RET_NOT_SUPPORTED;
    }
}

/* GD32 ADC 操作表。
   device 层调用 device_init/device_read/device_control 时，会通过这个表分发到底层函数。 */
static const device_ops_t s_gd32_adc_ops =
{
    gd32_adc_init,
    0,
    0,
    gd32_adc_read,
    0,
    gd32_adc_control,
};

/*****************************************************************************
@brief: 注册一个 GD32 ADC 扫描组及组内所有逻辑 ADC 设备
@para:cfg ADC 扫描组配置，由 BSP 提供
@return: RET_OK 表示注册成功，其他返回值表示失败
@note:
  一个扫描组对应一个物理 ADC + DMA 通道；组内每个通道注册为一个 device。
  这样上层可以通过名字读取单路 raw 值，而底层仍然使用连续扫描 DMA 提高效率。
*******************************************************************************/
int gd32_adc_register(const gd32_adc_cfg_t *cfg)
{
    gd32_adc_group_t *group;
    gd32_adc_slot_t *slot;
    int ret;
    uint8_t i;
    uint8_t j;

    if ((cfg == 0) || (cfg->channels == 0) || (cfg->channel_count == 0U) ||
        (cfg->channel_count > GD32_ADC_CHANNEL_MAX))
    {
        return RET_INVALID_PARAM;
    }

    if ((s_adc_group_count >= GD32_ADC_GROUP_MAX) ||
        ((uint32_t)s_adc_slot_count + (uint32_t)cfg->channel_count > (uint32_t)(GD32_ADC_GROUP_MAX * GD32_ADC_CHANNEL_MAX)))
    {
        return RET_NO_RESOURCE;
    }

    /* 注册前先检查名字合法性和重复性。
       这样可以避免前几个通道已经注册成功、后面通道才发现重名导致状态半完成。 */
    for (i = 0U; i < cfg->channel_count; i++)
    {
        if (cfg->channels[i].name == 0)
        {
            return RET_INVALID_PARAM;
        }

        if (device_find(cfg->channels[i].name) != 0)
        {
            return RET_ALREADY_EXISTS;
        }

        for (j = (uint8_t)(i + 1U); j < cfg->channel_count; j++)
        {
            if ((cfg->channels[j].name != 0) && (strcmp(cfg->channels[i].name, cfg->channels[j].name) == 0))
            {
                return RET_ALREADY_EXISTS;
            }
        }
    }

    /* 申请一个静态扫描组槽位，并保存 BSP 传入的硬件资源配置。 */
    group = &s_adc_groups[s_adc_group_count];
    group->adc_periph = cfg->adc_periph;
    group->adc_rcu = cfg->adc_rcu;
    group->adc_clock_div = cfg->adc_clock_div;
    group->dma_periph = cfg->dma_periph;
    group->dma_rcu = cfg->dma_rcu;
    group->dma_channel = cfg->dma_channel;
    group->channels = cfg->channels;
    group->channel_count = cfg->channel_count;
    group->initialized = 0U;

    /* 将扫描组内每个 ADC 通道注册成独立 device。
       user_data 指向 slot->adc，slot->adc 内部再记录所属 group 和 index。 */
    for (i = 0U; i < cfg->channel_count; i++)
    {
        group->samples[i] = 0U;
        slot = &s_adc_slots[s_adc_slot_count];
        slot->adc.group = group;
        slot->adc.index = i;
        slot->dev.name = cfg->channels[i].name;
        slot->dev.type = DEVICE_CLASS_ADC;
        slot->dev.ops = &s_gd32_adc_ops;
        slot->dev.user_data = &slot->adc;
        slot->dev.flags = 0U;
        slot->dev.initialized = 0U;
        slot->dev.opened = 0U;
        slot->dev.ref_count = 0U;

        ret = device_register(&slot->dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }

        s_adc_slot_count++;
    }

    s_adc_group_count++;

    return RET_OK;
}
