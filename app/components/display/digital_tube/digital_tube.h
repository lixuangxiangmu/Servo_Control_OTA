#ifndef DIGITAL_TUBE_H
#define DIGITAL_TUBE_H

#include "dev_gpio.h"

#include <stdint.h>

/* 四位八段数码管的段序，配置表必须按 A/B/C/D/E/F/G/DP 的顺序填入 GPIO 名称。 */
typedef enum
{
    DIGITAL_TUBE_SEG_A = 0,
    DIGITAL_TUBE_SEG_B,
    DIGITAL_TUBE_SEG_C,
    DIGITAL_TUBE_SEG_D,
    DIGITAL_TUBE_SEG_E,
    DIGITAL_TUBE_SEG_F,
    DIGITAL_TUBE_SEG_G,
    DIGITAL_TUBE_SEG_DP,
    DIGITAL_TUBE_SEGMENT_COUNT
} digital_tube_segment_t;

#define DIGITAL_TUBE_DIGIT_COUNT 4U

/* 数码管组件配置。硬件极性由应用配置层注入，组件本身不绑定具体板级引脚。 */
typedef struct
{
    const char *segment_gpio_names[DIGITAL_TUBE_SEGMENT_COUNT];  /* A/B/C/D/E/F/G/DP 段选 GPIO 名称。 */
    const char *digit_gpio_names[DIGITAL_TUBE_DIGIT_COUNT];      /* 从左到右 4 位位选 GPIO 名称。 */
    gpio_level_t segment_active_level;                           /* 段点亮时输出的电平，共阳极通常为低电平。 */
    gpio_level_t digit_active_level;                             /* 位选导通时输出的电平，按板级驱动电路配置。 */
} digital_tube_cfg_t;

/*****************************************************************************
@brief: 初始化数码管组件
@para:cfg 应用层注入的段选 GPIO、位选 GPIO 和有效电平配置
@return: RET_OK 表示初始化成功，其他返回值表示失败
@note: 该函数只保存配置并把所有段选、位选输出到熄灭状态，不创建任务或定时器。
*******************************************************************************/
int digital_tube_init(const digital_tube_cfg_t *cfg);

/*****************************************************************************
@brief: 打开或关闭数码管显示输出
@para:enable 非 0 表示允许扫描显示，0 表示关闭显示并立即熄灭所有段/位
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int digital_tube_set_enable(uint8_t enable);

/*****************************************************************************
@brief: 清空数码管显示缓冲区
@return: RET_OK 表示清空成功，其他返回值表示失败
@note: 该函数只修改显示缓冲区，实际熄灭动作会在下一次扫描刷新时体现。
*******************************************************************************/
int digital_tube_clear(void);

/*****************************************************************************
@brief: 设置单个数码管位的显示内容
@para:pos 位索引，0 表示最左侧，DIGITAL_TUBE_DIGIT_COUNT - 1 表示最右侧
@para:num 显示数字，范围 0-9
@para:dp_enable 非 0 表示点亮该位小数点，0 表示不点亮小数点
@return: RET_OK 表示设置成功，其他返回值表示失败
*******************************************************************************/
int digital_tube_set_digit(uint8_t pos, uint8_t num, uint8_t dp_enable);

/*****************************************************************************
@brief: 显示一个无符号整数
@para:number 待显示数字，范围 0-9999，超过 9999 时按 9999 显示
@return: RET_OK 表示设置成功，其他返回值表示失败
@note: 显示结果右对齐，并自动隐藏前导零。例如 12 显示为空、空、1、2。
*******************************************************************************/
int digital_tube_show_number(uint16_t number);

/*****************************************************************************
@brief: 执行一次数码管动态扫描刷新
@return: 无
@note: 每次只刷新一位，建议由 service 层以 1ms 周期调用。
*******************************************************************************/
void digital_tube_scan_once(void);

#endif
