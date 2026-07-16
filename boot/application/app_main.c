#include "app_main.h"

#include "board.h"
#include "board_config.h"
#include "version_config.h"
#include "app_config.h"
#include "eeprom.h"
#include "ota_flash.h"
#include "ota_image.h"
#include "ota_protocol.h"
#include "ota_service.h"
#include "return_code.h"
#include "app_mem_map.h"

#include "gd32f10x.h"

#include <stddef.h>
#include <string.h>

#define BOOT_APP_STACK_ALIGN_MASK        0x7UL
#define BOOT_APP_RESET_THUMB_MASK        0x1UL
#define BOOT_NVIC_REGISTER_WORDS         8U
#define BOOT_DMA1_CHANNEL_COUNT          5U

typedef void (*boot_app_entry_t)(void);

static const eeprom_cfg_t s_boot_eeprom_cfg =
{
    BOOT_EEPROM_DEVICE_NAME,
    BOARD_I2C0_NAME,
    EEPROM_TYPE_AT24C08,
    0x50U,
    20U,
    0U,
    0U,
    0U,
    0U,
};

static volatile uint32_t s_boot_ms;

static uint32_t boot_read_word(uint32_t addr)
{
    return *((volatile const uint32_t *)addr);
}

static uint8_t boot_addr_in_range(uint32_t addr, uint32_t base, uint32_t size)
{
    return ((addr >= base) && (addr < (base + size))) ? 1U : 0U;
}

uint8_t boot_app_is_valid(void)
{
    uint32_t app_sp = boot_read_word(CONFIG_APP_BASE_ADDR);
    uint32_t app_reset = boot_read_word(CONFIG_APP_BASE_ADDR + 4U);
    uint32_t app_reset_addr = app_reset & ~BOOT_APP_RESET_THUMB_MASK;
    uint32_t sram_end = CONFIG_SRAM_BASE_ADDR + CONFIG_SRAM_SIZE;

    /* APP 初始栈顶必须落在片内 SRAM 内，并保持 8 字节对齐。 */
    if ((app_sp < CONFIG_SRAM_BASE_ADDR) ||
        (app_sp > sram_end) ||
        ((app_sp & BOOT_APP_STACK_ALIGN_MASK) != 0U))
    {
        return 0U;
    }

    /*
     * Cortex-M3 复位向量最低位必须为 1，表示 Thumb 入口；
     * 去掉 Thumb 位后的真实地址必须落在 APP Flash 分区。
     */
    if (((app_reset & BOOT_APP_RESET_THUMB_MASK) == 0U) ||
        (boot_addr_in_range(app_reset_addr, CONFIG_APP_BASE_ADDR, CONFIG_APP_SIZE) == 0U))
    {
        return 0U;
    }

    return 1U;
}

static void boot_disable_dma(uint32_t dma_periph, uint32_t channel_count)
{
    uint32_t channel;

    for (channel = 0U; channel < channel_count; channel++)
    {
        dma_channel_disable(dma_periph, (dma_channel_enum)channel);
    }
}

static void boot_prepare_for_app_jump(void)
{
    uint32_t i;

    __disable_irq();

    /* 关闭 Boot 侧 SysTick，避免跳转后继续触发 Boot 的系统节拍。 */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    /* 关闭 Boot 已启用的 DMA1 通道，防止后台搬运继续写入 Boot 的缓冲区。 */
    boot_disable_dma(DMA1, BOOT_DMA1_CHANNEL_COUNT);

    /* 关闭并清除全部外设中断挂起位，让 APP 从干净的 NVIC 状态启动。 */
    for (i = 0U; i < BOOT_NVIC_REGISTER_WORDS; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
    __DSB();
    __ISB();
}

void boot_jump_to_app(void)
{
    uint32_t app_sp;
    uint32_t app_reset;
    boot_app_entry_t app_entry;

    if (boot_app_is_valid() == 0U)
    {
        return;
    }

    app_sp = boot_read_word(CONFIG_APP_BASE_ADDR);
    app_reset = boot_read_word(CONFIG_APP_BASE_ADDR + 4U);
    app_entry = (boot_app_entry_t)app_reset;

    boot_prepare_for_app_jump();

    /* 切换到 APP 向量表后设置 MSP，再按复位默认语义放开全局中断屏蔽。 */
    nvic_vector_table_set(NVIC_VECTTAB_FLASH, APP_VECT_TAB_OFFSET);
    __set_MSP(app_sp);
    __enable_irq();

    app_entry();

    while (1)
    {
    }
}


/*
 * 判断当前是否需要进入 OTA 模式。
 * 条件：EEPROM 中有有效的 OTA 请求标志，或 App 区镜像校验不通过。
 */
static void boot_record_app_failure(ota_eeprom_info_t *ota_info,
                                    uint8_t ota_info_valid,
                                    uint32_t fail_reason)
{
    if (ota_info == NULL)
    {
        return;
    }

    if (ota_info_valid == 0U)
    {
        /* Rebuild metadata when both EEPROM copies are invalid. */
        ota_eeprom_init_default(ota_info);
    }

    ota_info->ota_state = (uint32_t)OTA_STATE_FAILED;
    ota_info->app_valid = 0U;
    ota_info->app_confirmed = 0U;
    ota_info->fail_reason = fail_reason;
    (void)ota_eeprom_save(ota_info);
}

static uint8_t boot_need_ota_mode(ota_eeprom_info_t *ota_info,
                                  uint8_t ota_info_valid)
{
    if (ota_info_valid != 0U)
    {
        uint32_t state = ota_info->ota_state;

        if ((state == (uint32_t)OTA_STATE_REQUEST) ||
            (state == (uint32_t)OTA_STATE_DOWNLOADING) ||
            (state == (uint32_t)OTA_STATE_VERIFYING) ||
            (state == (uint32_t)OTA_STATE_FAILED))
        {
            return 1U;
        }
    }

    /* A valid image must pass vector-table and full-image CRC checks. */
    if (boot_app_is_valid() == 0U)
    {
        boot_record_app_failure(ota_info,
                                ota_info_valid,
                                (uint32_t)OTA_ERR_IMAGE_INVALID);
        return 1U;
    }

    if ((ota_info_valid != 0U) && (ota_info->app_valid != 0U) &&
        (ota_info->image_size > 0U))
    {
        uint32_t image_crc32;

        if ((ota_info->image_size > CONFIG_APP_SIZE) ||
            (ota_flash_calculate_crc32(ota_info->image_size,
                                       &image_crc32) != OTA_FLASH_OK))
        {
            boot_record_app_failure(ota_info,
                                    ota_info_valid,
                                    (uint32_t)OTA_ERR_IMAGE_INVALID);
            return 1U;
        }

        if (image_crc32 != ota_info->image_crc32)
        {
            boot_record_app_failure(ota_info,
                                    ota_info_valid,
                                    (uint32_t)OTA_ERR_IMAGE_CRC);
            return 1U;
        }
    }

    return 0U;
}


void boot_main(void)
{
    int ret;
    ota_eeprom_info_t ota_info;
    uint8_t ota_info_valid = 0U;

    board_init();

    ret = eeprom_register(&s_boot_eeprom_cfg);
    if (RET_IS_ERR(ret) && (ret != RET_ALREADY_EXISTS))
    {
        while (1)
        {
        }
    }

    /* 从 EEPROM 加载 OTA 状态，判断是否需要进入升级模式 */
    ret = ota_eeprom_load(&ota_info);
    if (RET_IS_OK(ret))
    {
        ota_info_valid = 1U;
    }

    if (boot_need_ota_mode(&ota_info, ota_info_valid))
    {
        ota_service_run(&ota_info, ota_info_valid);
    }

    /* 正常启动：校验 App 并跳转 */
    boot_jump_to_app();

    while (1)
    {
    }
}
