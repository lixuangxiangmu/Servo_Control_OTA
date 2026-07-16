#include "display/digital_tube/digital_tube.h"

#include "return_code.h"

#define DIGITAL_TUBE_PATTERN_A   (1U << DIGITAL_TUBE_SEG_A)
#define DIGITAL_TUBE_PATTERN_B   (1U << DIGITAL_TUBE_SEG_B)
#define DIGITAL_TUBE_PATTERN_C   (1U << DIGITAL_TUBE_SEG_C)
#define DIGITAL_TUBE_PATTERN_D   (1U << DIGITAL_TUBE_SEG_D)
#define DIGITAL_TUBE_PATTERN_E   (1U << DIGITAL_TUBE_SEG_E)
#define DIGITAL_TUBE_PATTERN_F   (1U << DIGITAL_TUBE_SEG_F)
#define DIGITAL_TUBE_PATTERN_G   (1U << DIGITAL_TUBE_SEG_G)
#define DIGITAL_TUBE_PATTERN_DP  (1U << DIGITAL_TUBE_SEG_DP)

/* 数字段码表。这里用“bit=1 表示该段需要点亮”的逻辑段码，真正输出电平由极性配置决定。 */
static const uint8_t s_digit_patterns[10] =
{
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C |
        DIGITAL_TUBE_PATTERN_D | DIGITAL_TUBE_PATTERN_E | DIGITAL_TUBE_PATTERN_F,
    DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_D |
        DIGITAL_TUBE_PATTERN_E | DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C |
        DIGITAL_TUBE_PATTERN_D | DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C | DIGITAL_TUBE_PATTERN_F |
        DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_C | DIGITAL_TUBE_PATTERN_D |
        DIGITAL_TUBE_PATTERN_F | DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_C | DIGITAL_TUBE_PATTERN_D |
        DIGITAL_TUBE_PATTERN_E | DIGITAL_TUBE_PATTERN_F | DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C |
        DIGITAL_TUBE_PATTERN_D | DIGITAL_TUBE_PATTERN_E | DIGITAL_TUBE_PATTERN_F |
        DIGITAL_TUBE_PATTERN_G,
    DIGITAL_TUBE_PATTERN_A | DIGITAL_TUBE_PATTERN_B | DIGITAL_TUBE_PATTERN_C |
        DIGITAL_TUBE_PATTERN_D | DIGITAL_TUBE_PATTERN_F | DIGITAL_TUBE_PATTERN_G,
};

static digital_tube_cfg_t s_tube_cfg;
static uint8_t s_display_patterns[DIGITAL_TUBE_DIGIT_COUNT];
static uint8_t s_current_pos;
static uint8_t s_initialized;
static uint8_t s_enabled;

/*****************************************************************************
@brief: 根据有效电平计算无效电平
@para:active_level 某一路 GPIO 被认为“有效”时需要输出的电平
@return: 与 active_level 相反的电平，用于关闭段选或位选
*******************************************************************************/
static gpio_level_t digital_tube_inactive_level(gpio_level_t active_level)
{
    return (active_level == GPIO_LEVEL_HIGH) ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
}

/*****************************************************************************
@brief: 检查数码管组件配置是否合法
@para:cfg 数码管组件配置指针
@return: 1 表示配置有效，0 表示配置无效
@note: 这里仅检查指针、GPIO 名称和电平枚举是否合法；GPIO 是否已经注册由 dev_gpio 层返回错误。
*******************************************************************************/
static int digital_tube_cfg_is_valid(const digital_tube_cfg_t *cfg)
{
    uint8_t i;

    if ((cfg == 0) ||
        (cfg->segment_active_level > GPIO_LEVEL_HIGH) ||
        (cfg->digit_active_level > GPIO_LEVEL_HIGH))
    {
        return 0;
    }

    for (i = 0U; i < (uint8_t)DIGITAL_TUBE_SEGMENT_COUNT; i++)
    {
        if (cfg->segment_gpio_names[i] == 0)
        {
            return 0;
        }
    }

    for (i = 0U; i < DIGITAL_TUBE_DIGIT_COUNT; i++)
    {
        if (cfg->digit_gpio_names[i] == 0)
        {
            return 0;
        }
    }

    return 1;
}

/*****************************************************************************
@brief: 将所有位选 GPIO 写成同一个电平
@para:level 需要输出到所有位选 GPIO 的电平
@return: RET_OK 表示写入成功，其他返回值表示失败
@note: 动态扫描前先关闭所有位选，可以避免切换段码时产生重影。
*******************************************************************************/
static int digital_tube_write_all_digits(gpio_level_t level)
{
    uint8_t i;
    int ret;

    for (i = 0U; i < DIGITAL_TUBE_DIGIT_COUNT; i++)
    {
        ret = dev_gpio_write(s_tube_cfg.digit_gpio_names[i], level);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 按逻辑段码刷新所有段选 GPIO
@para:pattern 逻辑段码，bit=1 表示对应段需要点亮，bit=0 表示对应段熄灭
@return: RET_OK 表示写入成功，其他返回值表示失败
@note: 逻辑段码不直接代表 GPIO 电平，最终电平由 segment_active_level 决定。
*******************************************************************************/
static int digital_tube_write_segments(uint8_t pattern)
{
    uint8_t i;
    gpio_level_t inactive_level;
    gpio_level_t level;
    int ret;

    inactive_level = digital_tube_inactive_level(s_tube_cfg.segment_active_level);

    for (i = 0U; i < (uint8_t)DIGITAL_TUBE_SEGMENT_COUNT; i++)
    {
        level = ((pattern & (uint8_t)(1U << i)) != 0U) ?
                s_tube_cfg.segment_active_level : inactive_level;

        ret = dev_gpio_write(s_tube_cfg.segment_gpio_names[i], level);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 熄灭所有数码管输出
@return: RET_OK 表示熄灭成功，其他返回值表示失败
@note: 先关闭所有位选，再关闭段选，避免关闭过程中出现短暂误亮。
*******************************************************************************/
static int digital_tube_blank_outputs(void)
{
    int ret;

    ret = digital_tube_write_all_digits(digital_tube_inactive_level(s_tube_cfg.digit_active_level));
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return digital_tube_write_segments(0U);
}

/*****************************************************************************
@brief: 初始化数码管组件
@para:cfg 段选、位选 GPIO 名称和有效电平配置
@return: RET_OK 表示成功，其它返回值表示失败
@note: 初始化后显示处于关闭状态，显示缓冲区全部清空；需要调用 digital_tube_set_enable() 后才会扫描点亮。
*******************************************************************************/
int digital_tube_init(const digital_tube_cfg_t *cfg)
{
    uint8_t i;
    int ret;

    if (digital_tube_cfg_is_valid(cfg) == 0)
    {
        return RET_INVALID_PARAM;
    }

    s_tube_cfg = *cfg;
    s_current_pos = 0U;
    s_enabled = 0U;

    for (i = 0U; i < DIGITAL_TUBE_DIGIT_COUNT; i++)
    {
        s_display_patterns[i] = 0U;
    }

    s_initialized = 1U;

    ret = digital_tube_blank_outputs();
    if (RET_IS_ERR(ret))
    {
        s_initialized = 0U;
        return ret;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 打开或关闭数码管显示
@para:enable 非 0 表示开启显示，0 表示关闭显示并熄灭所有段/位
@return: RET_OK 表示成功，其它返回值表示失败
@note: 该函数只控制组件是否允许扫描点亮，不负责创建或启动定时器。
*******************************************************************************/
int digital_tube_set_enable(uint8_t enable)
{
    if (s_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    s_enabled = (enable != 0U) ? 1U : 0U;

    if (s_enabled == 0U)
    {
        return digital_tube_blank_outputs();
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 清空显示缓冲区
@return: RET_OK 表示成功，其它返回值表示失败
@note: 清空后各位逻辑段码为 0；如果显示仍处于开启状态，下一轮扫描会显示为空白。
*******************************************************************************/
int digital_tube_clear(void)
{
    uint8_t i;

    if (s_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    for (i = 0U; i < DIGITAL_TUBE_DIGIT_COUNT; i++)
    {
        s_display_patterns[i] = 0U;
    }

    return RET_OK;
}

/*****************************************************************************
@brief: 设置单个数码管位的显示数字
@para:pos 位置索引，0 表示最左侧，3 表示最右侧
@para:num 数字 0-9
@para:dp_enable 是否点亮该位的小数点
@return: RET_OK 表示成功，其它返回值表示失败
@note: 该接口只更新显示缓冲区，不会立即阻塞等待当前位刷新完成。
*******************************************************************************/
int digital_tube_set_digit(uint8_t pos, uint8_t num, uint8_t dp_enable)
{
    uint8_t pattern;

    if (s_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    if ((pos >= DIGITAL_TUBE_DIGIT_COUNT) || (num >= 10U))
    {
        return RET_INVALID_PARAM;
    }

    pattern = s_digit_patterns[num];
    if (dp_enable != 0U)
    {
        pattern |= DIGITAL_TUBE_PATTERN_DP;
    }

    s_display_patterns[pos] = pattern;

    return RET_OK;
}

/*****************************************************************************
@brief: 显示 0-9999 的无符号整数，自动右对齐并隐藏前导零
@para:number 待显示的数字，大于 9999 时按 9999 显示
@return: RET_OK 表示成功，其它返回值表示失败
@note: 数字 0 会显示在最右侧一位；例如 180 会显示为空、1、8、0。
*******************************************************************************/
int digital_tube_show_number(uint16_t number)
{
    int8_t pos;

    if (s_initialized == 0U)
    {
        return RET_NOT_INITED;
    }

    if (number > 9999U)
    {
        number = 9999U;
    }

    for (pos = (int8_t)DIGITAL_TUBE_DIGIT_COUNT - 1; pos >= 0; pos--)
    {
        s_display_patterns[pos] = 0U;
    }

    pos = (int8_t)DIGITAL_TUBE_DIGIT_COUNT - 1;
    do
    {
        s_display_patterns[pos] = s_digit_patterns[number % 10U];
        number = (uint16_t)(number / 10U);
        pos--;
    } while ((number > 0U) && (pos >= 0));

    return RET_OK;
}

/*****************************************************************************
@brief: 扫描刷新一次数码管，调用周期建议为 1ms
@return: 无
@note: 刷新顺序为关闭所有位选、设置当前位段码、打开当前位选、移动到下一位。
       该函数不应长时间阻塞，外层服务通过 FreeRTOS 软件定时器周期调用。
*******************************************************************************/
void digital_tube_scan_once(void)
{
    gpio_level_t digit_inactive_level;

    if (s_initialized == 0U)
    {
        return;
    }

    digit_inactive_level = digital_tube_inactive_level(s_tube_cfg.digit_active_level);

    (void)digital_tube_write_all_digits(digit_inactive_level);

    if (s_enabled == 0U)
    {
        (void)digital_tube_write_segments(0U);
        return;
    }

    (void)digital_tube_write_segments(s_display_patterns[s_current_pos]);
    (void)dev_gpio_write(s_tube_cfg.digit_gpio_names[s_current_pos], s_tube_cfg.digit_active_level);

    s_current_pos++;
    if (s_current_pos >= DIGITAL_TUBE_DIGIT_COUNT)
    {
        s_current_pos = 0U;
    }
}
