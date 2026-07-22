/*****************************************************************************
 * @file    app_main.c
 * @brief   Bootloader application validation, preparation, and entry point
 *****************************************************************************/

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

/** Mask applied to the initial stack pointer — must be 8-byte aligned per ARM AAPCS. */
#define BOOT_APP_STACK_ALIGN_MASK        0x7UL
/** Mask to test the Thumb-mode bit (bit 0) in a Cortex-M3 reset vector. */
#define BOOT_APP_RESET_THUMB_MASK        0x1UL
/** Number of 32-bit words in the NVIC interrupt-control register array (0–239). */
#define BOOT_NVIC_REGISTER_WORDS         8U
/** Number of DMA1 channels present on the GD32F103 series. */
#define BOOT_DMA1_CHANNEL_COUNT          5U

/** Function pointer to the application's reset handler (Thumb-mode entry). */
typedef void (*boot_app_entry_t)(void);

/** EEPROM configuration for the bootloader's persistent OTA state block. */
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

/** Monotonically-incrementing millisecond counter updated by the SysTick ISR. */
static volatile uint32_t s_boot_ms;

/**
 * @brief Read a 32-bit word from an absolute memory address.
 *
 * Used to peek at the application's vector table without dereferencing a
 * C pointer, which the compiler might optimize away for flash-mapped regions.
 *
 * @param addr Absolute byte address to read.
 * @return The 32-bit value stored at that address.
 */
static uint32_t boot_read_word(uint32_t addr)
{
    return *((volatile const uint32_t *)addr);
}

/**
 * @brief Check whether an absolute address falls within a contiguous region.
 *
 * @param addr Address to test.
 * @param base Region start address (inclusive).
 * @param size Region size in bytes.
 * @return 1 if @p addr is within [base, base + size); otherwise 0.
 */
static uint8_t boot_addr_in_range(uint32_t addr, uint32_t base, uint32_t size)
{
    return ((addr >= base) && (addr < (base + size))) ? 1U : 0U;
}

/**
 * @brief Validate the application image's vector table.
 *
 * Verifies two properties required by the Cortex-M3 architecture:
 * -# The initial stack pointer must fall within the on-chip SRAM region and
 *    satisfy 8-byte stack alignment (AAPCS requirement).
 * -# The reset vector must have its Thumb-mode bit set (bit 0 = 1) and,
 *    after masking that bit, the target address must lie inside the
 *    application Flash partition.
 *
 * @return 1 when both vector entries are architecturally valid; otherwise 0.
 */
uint8_t boot_app_is_valid(void)
{
    uint32_t app_sp = boot_read_word(CONFIG_APP_BASE_ADDR);
    uint32_t app_reset = boot_read_word(CONFIG_APP_BASE_ADDR + 4U);
    uint32_t app_reset_addr = app_reset & ~BOOT_APP_RESET_THUMB_MASK;
    uint32_t sram_end = CONFIG_SRAM_BASE_ADDR + CONFIG_SRAM_SIZE;

    /* The initial stack pointer must reside in on-chip SRAM and be 8-byte aligned. */
    if ((app_sp < CONFIG_SRAM_BASE_ADDR) ||
        (app_sp > sram_end) ||
        ((app_sp & BOOT_APP_STACK_ALIGN_MASK) != 0U))
    {
        return 0U;
    }

    /*
     * Cortex-M3 reset vectors must have bit 0 set to indicate a Thumb-mode
     * entry point. After clearing that bit, the resolved address must fall
     * within the application Flash region.
     */
    if (((app_reset & BOOT_APP_RESET_THUMB_MASK) == 0U) ||
        (boot_addr_in_range(app_reset_addr, CONFIG_APP_BASE_ADDR, CONFIG_APP_SIZE) == 0U))
    {
        return 0U;
    }

    return 1U;
}

/**
 * @brief Prepare the Cortex-M3 system state for a clean handover to the application.
 *
 * Disables interrupts globally, stops SysTick, resets and gates every
 * Bootloader-owned peripheral, clears all NVIC interrupt-enable and pending
 * registers, and resets the system-control block's PendSV and SysTick
 * exception state. A DSB/ISB barrier pair ensures the changes are visible
 * before the jump.
 */
static void boot_prepare_for_app_jump(void)
{
    uint32_t i;

    __disable_irq();

    /* Stop bootloader SysTick so the application does not inherit a running timer. */
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;

    /* Reset the UART3/I2C0/DMA1 resources initialized by board_init(). */
    board_deinit_for_app_jump();

    /* delay_init() enables the DWT cycle counter. Restore its reset-state
       behaviour so the App does not inherit Bootloader debug/trace state. */
    DWT->CTRL &= ~DWT_CTRL_CYCCNTENA_Msk;
    DWT->CYCCNT = 0U;
    CoreDebug->DEMCR &= ~CoreDebug_DEMCR_TRCENA_Msk;

    /* Clear all peripheral interrupt enable and pending bits so the
       application starts from a known-clean NVIC state. */
    for (i = 0U; i < BOOT_NVIC_REGISTER_WORDS; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    /* Clear PendSV and SysTick exception state in the system control block. */
    SCB->ICSR = SCB_ICSR_PENDSVCLR_Msk | SCB_ICSR_PENDSTCLR_Msk;
    __DSB();
    __ISB();
}

/**
 * @brief Validate the application image and transfer control to it.
 *
 * Reads the application's initial stack pointer and reset vector from the
 * vector table at CONFIG_APP_BASE_ADDR. After a full system-state cleanup,
 * the NVIC vector table is relocated, the main stack pointer is set, and
 * interrupts are re-enabled with __enable_irq() — matching the default
 * reset behaviour — before branching to the application's entry point.
 */
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

    /* Switch to the application vector table, set MSP from the
       application's initial stack pointer, then restore the default
       post-reset interrupt-enable state before entry. */
    nvic_vector_table_set(NVIC_VECTTAB_FLASH, APP_VECT_TAB_OFFSET);
    __set_MSP(app_sp);
    __enable_irq();

    app_entry();

    /* The application entry must never return; spin if it does. */
    while (1)
    {
    }
}


/**
 * @brief Record a terminal application-validation failure in OTA metadata.
 *
 * When both EEPROM copies are invalid, default metadata is rebuilt first so
 * there is always a valid structure to persist the failure against.
 *
 * @param ota_info        Destination OTA metadata structure.
 * @param ota_info_valid  Non-zero when @p ota_info holds valid EEPROM data.
 * @param fail_reason     Protocol-defined failure code to record.
 */
static void boot_record_app_failure(ota_eeprom_info_t *ota_info, uint8_t ota_info_valid, uint32_t fail_reason)
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

/**
 * @brief Determine whether the bootloader must enter OTA mode.
 *
 * OTA mode is required when:
 * - The EEPROM contains an unresolved OTA request, a download in progress,
 *   a pending verification, or a terminal failure; or
 * - The application vector table is structurally invalid; or
 * - The stored image size and CRC32 do not match a full readback of the
 *   programmed application Flash.
 *
 * @param ota_info       OTA metadata loaded from EEPROM.
 * @param ota_info_valid Non-zero when @p ota_info was successfully loaded.
 * @return 1 if OTA mode is required; otherwise 0 (boot the application).
 */
static uint8_t boot_need_ota_mode(ota_eeprom_info_t *ota_info, uint8_t ota_info_valid)
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
        boot_record_app_failure(ota_info, ota_info_valid, (uint32_t)OTA_ERR_IMAGE_INVALID);
        return 1U;
    }

    if ((ota_info_valid != 0U) && (ota_info->app_valid != 0U) && (ota_info->image_size > 0U))
    {
        uint32_t image_crc32;

        if ((ota_info->image_size > CONFIG_APP_SIZE) || (ota_flash_calculate_crc32(ota_info->image_size, &image_crc32) != OTA_FLASH_OK))
        {
            boot_record_app_failure(ota_info, ota_info_valid, (uint32_t)OTA_ERR_IMAGE_INVALID);
            return 1U;
        }

        if (image_crc32 != ota_info->image_crc32)
        {
            boot_record_app_failure(ota_info, ota_info_valid, (uint32_t)OTA_ERR_IMAGE_CRC);
            return 1U;
        }
    }

    return 0U;
}


/**
 * @brief Bootloader main entry point.
 *
 * Initializes the hardware, registers the EEPROM device, loads the
 * persistent OTA state, and decides whether to enter the OTA service loop
 * or jump directly to the application. If the OTA service loop exits,
 * control falls through to the normal application boot path.
 */
void boot_main(void)
{
    int ret;
    ota_eeprom_info_t ota_info;
    uint8_t ota_info_valid = 0U;

    board_init();

    ret = eeprom_register(&s_boot_eeprom_cfg);
    if (RET_IS_ERR(ret) && (ret != RET_ALREADY_EXISTS))
    {
        /* EEPROM registration failed for a reason other than a duplicate;
           halt here — further OTA persistence cannot work. */
        while (1)
        {
        }
    }

    /* Load the persistent OTA state from EEPROM to decide whether an
       upgrade cycle is in progress. */
    ret = ota_eeprom_load(&ota_info);
    if (RET_IS_OK(ret))
    {
        ota_info_valid = 1U;
    }

    if (boot_need_ota_mode(&ota_info, ota_info_valid))
    {
        ota_service_task(&ota_info, ota_info_valid);
    }

    /* Normal boot path: validate and jump to the application. */
    boot_jump_to_app();

    /* boot_jump_to_app must never return; spin if it does. */
    while (1)
    {
    }
}
