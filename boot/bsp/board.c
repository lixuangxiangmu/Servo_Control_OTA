/*****************************************************************************
 * @file    board.c
 * @brief   Bootloader 板级硬件初始化
 *
 * 仅注册 Bootloader 必需的外设:
 *   - UART3 (DMA + IDLE, 115200 8N1): 蓝牙 SPP 透传
 *   - I2C0 (硬件 I2C, 400kHz): 板载 EEPROM
 *
 * 系统时钟 (108MHz) 由 SystemInit() 在进入 main() 前完成。
 *****************************************************************************/

#include "board.h"

#include "board_config.h"
#include "device.h"
#include "drv_flash.h"
#include "drv_gpio.h"
#include "drv_i2c.h"
#include "drv_uart.h"
#include "project_config.h"
#include "return_code.h"
#include "app_mem_map.h"
#include "delay.h"
#include <stdint.h>

#define BOARD_ARRAY_SIZE(array) ((uint32_t)(sizeof(array) / sizeof((array)[0])))
/* =====================================================================
 * UART3 资源配置
 * TX: PC10, RX: PC11, 115200 8N1
 * DMA1: RX=CH2, TX=CH4
 * ===================================================================== */

static const gd32_uart_cfg_t s_board_uart =
{
    BOARD_UART3_NAME,
    UART3,                                /* uart_periph */
    RCU_UART3,                            /* uart_rcu */
    RCU_GPIOC,                            /* gpio_rcu */
    GPIOC,                                /* gpio_periph */
    GPIO_PIN_10,                          /* tx_pin */
    GPIO_PIN_11,                          /* rx_pin */
    UART3_IRQn,                           /* uart_irq */
    DMA1,                                 /* dma_periph */
    RCU_DMA1,                             /* dma_rcu */
    DMA_CH2,                              /* dma_rx_channel */
    DMA_CH4,                              /* dma_tx_channel */
    DMA1_Channel3_Channel4_IRQn,          /* dma_tx_irq */
    6U,                                   /* irq_preemption_priority */
    0U,                                   /* irq_sub_priority */
    {
        115200U,                           /* baudrate */
        UART_DATA_BITS_8,                  /* data_bits */
        UART_STOP_BITS_1,                  /* stop_bits */
        UART_PARITY_NONE                   /* parity */
    },
};

/* =====================================================================
 * I2C0 资源配置
 * SCL: PB6, SDA: PB7, 400kHz
 * ===================================================================== */

static const gd32_i2c_cfg_t s_board_i2c =
{
    BOARD_I2C0_NAME,                       /* name */
    I2C0,                                  /* i2c_periph */
    RCU_I2C0,                              /* i2c_rcu */
    RCU_GPIOB,                             /* scl_gpio_rcu */
    GPIOB,                                 /* scl_gpio_periph */
    GPIO_PIN_6,                            /* scl_gpio_pin */
    RCU_GPIOB,                             /* sda_gpio_rcu */
    GPIOB,                                 /* sda_gpio_periph */
    GPIO_PIN_7,                            /* sda_gpio_pin */
    {
        400000U,                            /* speed_hz */
        I2C_ADDR_7BIT,                      /* addr_mode */
        0x0AU                               /* own_addr */
    },
};

/* On-chip Flash: read the whole chip, write only APP + Reserve. */
static const gd32_flash_cfg_t s_board_flash =
{
    BOARD_FLASH_NAME,
    CONFIG_FLASH_BASE_ADDR,
    (CONFIG_BOOT_SIZE + CONFIG_APP_SIZE + RESERVE_SIZE),
    FLASH_PAGE_SIZE,
    CONFIG_APP_BASE_ADDR,
    (CONFIG_APP_SIZE + RESERVE_SIZE),
};

/* =====================================================================
 * 设备注册辅助
 * ===================================================================== */

/*****************************************************************************
 * @brief: 注册并初始化 UART 设备
 *****************************************************************************/
static void board_uart_register(void)
{
    device_t *dev;

    if (gd32_uart_register(&s_board_uart) == RET_OK)
    {
        dev = device_find(s_board_uart.name);
        (void)device_init(dev);
    }
}

/*****************************************************************************
 * @brief: 注册并初始化 I2C 设备
 *****************************************************************************/
static void board_i2c_register(void)
{
    device_t *dev;

    if (gd32_i2c_register(&s_board_i2c) == RET_OK)
    {
        dev = device_find(s_board_i2c.name);
        (void)device_init(dev);
    }
}

static void board_flash_register(void)
{
    device_t *dev;

    if (gd32_flash_register(&s_board_flash) == RET_OK)
    {
        dev = device_find(s_board_flash.name);
        (void)device_init(dev);
    }
}


static void board_systick_init(void)
{
    uint32_t reload = SystemCoreClock / 1000U;

    SysTick->CTRL = 0U;
    SysTick->LOAD = (reload == 0U) ? 0U : (reload - 1U);
    SysTick->VAL = 0U;
    SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/*****************************************************************************
 * @brief: 复位并关闭 Bootloader 已使用的 UART、I2C 及 DMA 硬件资源
 *
 * UART3 的 DMA 请求必须先关闭，再复位 DMA 通道和 UART，防止在 App 完成
 * 自身初始化前继续访问 Bootloader 的 DMA 缓冲区。I2C0 在复位前先请求 STOP，
 * 尽可能释放 EEPROM 总线。
 *****************************************************************************/
void board_deinit_for_app_jump(void)
{
    /* UART3: stop peripheral requests before resetting its DMA channels. */
    usart_interrupt_disable(UART3, USART_INT_IDLE);
    usart_dma_receive_config(UART3, USART_RECEIVE_DMA_DISABLE);
    usart_dma_transmit_config(UART3, USART_TRANSMIT_DMA_DISABLE);
    usart_disable(UART3);
    dma_deinit(DMA1, DMA_CH2);
    dma_deinit(DMA1, DMA_CH4);
    usart_deinit(UART3);

    /* I2C0 uses blocking transfers, but send STOP in case an error path left
       the peripheral in a bus-active state before its peripheral reset. */
    i2c_stop_on_bus(I2C0);
    i2c_disable(I2C0);
    i2c_deinit(I2C0);

    /* The flash driver locks FMC after every operation; lock once more so a
       future driver change cannot leave program/erase access enabled. */
    fmc_lock();

    /* No Bootloader code executes after this point, so gate its clocks. GPIO
       mode registers retain their values; the App BSP reconfigures all pins it
       owns before use. */
    rcu_periph_clock_disable(RCU_UART3);
    rcu_periph_clock_disable(RCU_I2C0);
    rcu_periph_clock_disable(RCU_DMA1);
    rcu_periph_clock_disable(RCU_GPIOB);
    rcu_periph_clock_disable(RCU_GPIOC);
}

/* =====================================================================
 * 公共接口
 * ===================================================================== */

/*****************************************************************************
 * @brief: 板级硬件初始化
 *****************************************************************************/
void board_init(void)
{
    /* NVIC 优先级分组: 4 位抢占优先级, 0 位子优先级。 */
    nvic_priority_group_set(NVIC_PRIGROUP_PRE4_SUB0);

    delay_init();   //DWT延时
    board_systick_init();

    /* 注册 Bootloader 所需外设。 */
    board_flash_register();
    board_uart_register();
    board_i2c_register();
}
