/*****************************************************************************
 * @file    ota_eeprom.h
 * @brief   OTA EEPROM A/B 双备份元数据读写模块
 *
 * 在 EEPROM 中保存两份 OTA 运行时信息，每次写入交替更新 A/B 副本，
 * 防止写 EEPROM 时意外掉电导致状态损坏。
 * 读取时通过 magic + struct_crc32 + seq 自动选择有效的最新副本。
 *
 * 本模块仅依赖 eeprom 抽象接口和 CRC32 工具函数，与具体芯片无关。
 *****************************************************************************/

#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H

#include "app_data_store.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


/* OTA EEPROM 魔数 "OTA1" = 0x4F544131 */
#define OTA_EE_MAGIC            0x4F544131UL
#define OTA_EE_STRUCT_VERSION   0x0001U
#define OTA_EE_STRUCT_SIZE      64U

/*****************************************************************************
 * @brief:  从 EEPROM 加载有效的 OTA 元数据
 * @param:  info   [输出] 加载成功后存放 OTA 元数据
 * @return: RET_OK 加载成功，其他值表示没有有效副本
 * @note:   自动读取 A/B 两个副本，通过 magic + struct_crc32 + seq 仲裁
 *****************************************************************************/
int ota_eeprom_load(ota_eeprom_info_t *info);

/*****************************************************************************
 * @brief:  保存 OTA 元数据到 EEPROM
 * @param:  info   [输入] 待保存的 OTA 元数据指针
 * @return: RET_OK 保存成功，其他值表示写入失败
 * @note:   内部自动交替写入 A/B 副本，seq 自增，自动计算 struct_crc32。
 *          如果 A/B 两个副本均无效，优先写入 A 副本。
 *****************************************************************************/
int ota_eeprom_save(const ota_eeprom_info_t *info);

/*****************************************************************************
 * @brief:  用安全默认值初始化 OTA 元数据结构体
 * @param:  info   [输出] 待初始化的结构体指针
 * @note:   填充后需调用 ota_eeprom_save 写入 EEPROM
 *****************************************************************************/
void ota_eeprom_init_default(ota_eeprom_info_t *info);

/*****************************************************************************
 * @brief:  计算 OTA 元数据结构体的自身 CRC32
 * @param:  info   [输入] OTA 元数据指针
 * @return: 对 struct_crc32 字段之前所有字节计算的 CRC32 值
 * @note:   调用方写入 EEPROM 前应调用此函数填充 struct_crc32
 *****************************************************************************/
uint32_t ota_eeprom_calc_struct_crc(const ota_eeprom_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* OTA_IMAGE_H */
