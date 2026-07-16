#ifndef __APP_MAM_MAP_H__
#define __APP_MAM_MAP_H__

#include "app_data_store.h"

/************************** Flash 地址***************************/
/* On-chip Flash base address. */
#define CONFIG_FLASH_BASE_ADDR      0x08000000UL

/* OTA Flash partition layout. */
#define CONFIG_BOOT_BASE_ADDR       0x08000000UL
#define CONFIG_BOOT_SIZE            0x00008000UL        //boot大小 32K
#define CONFIG_APP_BASE_ADDR        0x08008000UL        //app起始地址
#define CONFIG_APP_SIZE             0x00020000UL        //app大小 128K
#define CONFIG_APP_END_ADDR         (CONFIG_APP_BASE_ADDR + CONFIG_APP_SIZE - 1UL)
#define CONFIG_RESERVE_BASE_ADDR    (CONFIG_APP_BASE_ADDR + CONFIG_APP_SIZE)

/* GD32F103RCT6 high-density Flash erase page size. */
#define FLASH_PAGE_SIZE             2048UL

/* On-chip SRAM base address. */
#define CONFIG_SRAM_BASE_ADDR       0x20000000UL
#define CONFIG_SRAM_SIZE            0x0000C000UL
#define CONFIG_SRAM_END_ADDR        (CONFIG_SRAM_BASE_ADDR + CONFIG_SRAM_SIZE - 1UL)



/************************** EEPROM 起始地址***************************/
#define EE_START_ADDR ((uint32_t)(0x00000000))

#define EE_SERVO_DATA_ADDR          (EE_START_ADDR + offset_of(st_eeprom_allocate, servo_data))
#define EE_SERVO_SPEED              (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, servo_speed))
#define EE_SERVO_ANGLE_FOLD         (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, threshold_angle_fold))
#define EE_SERVO_ANGLE_INIT         (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, threshold_angle_init))
#define EE_KEY_LOCK_SEC             (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, enter_lock_sec))
#define EE_KEY_UNLOCK_SEC           (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, enter_unlock_sec))
#define EE_POWER_OFF_RETURN         (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, power_off_return))
#define EE_POWER_OFF_FOLD_STATE     (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, power_off_fold_state))
#define EE_MEMORY_MODE_FLAG         (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, memory_function_flag))
#define EE_UNLOCK_MODE              (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, unlock_mode))
#define EE_REMOTE_KEY_CODE          (EE_SERVO_DATA_ADDR + offset_of(st_eeprom_servo_data, remote_key_code))
#define EE_REMOTE_KEY_CODE_LOW      (EE_REMOTE_KEY_CODE)
#define EE_REMOTE_KEY_CODE_HIGH     (EE_REMOTE_KEY_CODE + 1U)

#define EE_OTA_DATA_ADDR            (EE_START_ADDR + offset_of(st_eeprom_allocate, ota_data))
#define EE_OTA_MAIN_DATA_ADDR       (EE_OTA_DATA_ADDR + offset_of(st_eeprom_ota_data, ota_main_data))
#define EE_OTA_BACK_DATA_ADDR       (EE_OTA_DATA_ADDR + offset_of(st_eeprom_ota_data, ota_back_data))

#endif
