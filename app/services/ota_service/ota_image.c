#include "ota_image.h"

#include "app_mem_map.h"
#include "eeprom.h"
#include "return_code.h"
#include "utils_lib.h"

#include <stddef.h>
#include <string.h>

/* OTA 元数据使用的 EEPROM 设备名称，需与 board_config.h 注册名称一致。 */
#define OTA_EEPROM_DEVICE_NAME   "eeprom0"

/* ---------------------------------------------------------------------------
 * 内部辅助：从 EEPROM 地址读取一份 OTA 副本，并校验其有效性。
 * 返回 1 表示 copy 有效，0 表示无效。
 * ------------------------------------------------------------------------- */
static int ota_eeprom_read_copy(uint32_t addr, ota_eeprom_info_t *info)
{
    int ret;

    ret = eeprom_read(OTA_EEPROM_DEVICE_NAME, addr, (uint8_t *)info, sizeof(ota_eeprom_info_t));
    if (ret != (int)sizeof(ota_eeprom_info_t))
    {
        return 0;
    }

    /* 魔数校验 */
    if (info->magic != OTA_EE_MAGIC)
    {
        return 0;
    }

    /* 结构体大小合理性检查 */
    if (info->struct_size != OTA_EE_STRUCT_SIZE)
    {
        return 0;
    }

    /* 结构体 CRC32 校验 */
    if (ota_eeprom_calc_struct_crc(info) != info->struct_crc32)
    {
        return 0;
    }

    return 1;
}


/* =====================================================================
 * 公开接口
 * ===================================================================== */

int ota_eeprom_load(ota_eeprom_info_t *load_info)
{
    ota_eeprom_info_t copy_a, copy_b;
    int a_valid, b_valid;

    if (load_info == NULL)
    {
        return RET_INVALID_PARAM;
    }

    a_valid = ota_eeprom_read_copy(EE_OTA_MAIN_DATA_ADDR, &copy_a);
    b_valid = ota_eeprom_read_copy(EE_OTA_BACK_DATA_ADDR, &copy_b);

    if (a_valid && b_valid)
    {
        /* 两个副本均有效，取 seq 较大者 */
        *load_info = (copy_a.seq >= copy_b.seq) ? copy_a : copy_b;
        return RET_OK;
    }
    else if (a_valid)
    {
        *load_info = copy_a;
        return RET_OK;
    }
    else if (b_valid)
    {
        *load_info = copy_b;
        return RET_OK;
    }

    return RET_FAIL;
}


int ota_eeprom_save(const ota_eeprom_info_t *info)
{
    ota_eeprom_info_t copy_a, copy_b;
    ota_eeprom_info_t write_info;
    int a_valid, b_valid;
    uint32_t target_addr;
    uint32_t current_max_seq;
    int ret;

    if (info == NULL)
    {
        return RET_INVALID_PARAM;
    }

    /* 读取当前 A/B 两个副本，确定写入目标和 seq 基数 */
    a_valid = ota_eeprom_read_copy(EE_OTA_MAIN_DATA_ADDR, &copy_a);
    b_valid = ota_eeprom_read_copy(EE_OTA_BACK_DATA_ADDR, &copy_b);

    /* 确定当前最高 seq */
    current_max_seq = 0U;
    if (a_valid)
    {
        current_max_seq = copy_a.seq;
    }
    if (b_valid && (copy_b.seq > current_max_seq))
    {
        current_max_seq = copy_b.seq;
    }

    /*
     * 选择要写入的副本：
     * - 如果只有一个副本有效，写入另一个副本（均衡磨损 + 防掉电损坏）
     * - 如果两个都有效，写入 seq 较小的副本（或 seq 相等时写入 B）
     * - 如果都无效，写入 A 副本（首次初始化场景）
     */
    if (a_valid && b_valid)
    {
        /* 两个都有效：覆盖 seq 较小者 */
        if (copy_a.seq < copy_b.seq)
        {
            target_addr = EE_OTA_MAIN_DATA_ADDR;   /* 覆盖 A */
        }
        else
        {
            target_addr = EE_OTA_BACK_DATA_ADDR;   /* 覆盖 B */
        }
    }
    else if (a_valid)
    {
        target_addr = EE_OTA_BACK_DATA_ADDR;       /* A 有效，写 B */
    }
    else if (b_valid)
    {
        target_addr = EE_OTA_MAIN_DATA_ADDR;       /* B 有效，写 A */
    }
    else
    {
        target_addr = EE_OTA_MAIN_DATA_ADDR;       /* 均无效，优先写 A */
    }

    /* 准备写入数据：复制输入，更新元数据字段 */
    memcpy(&write_info, info, sizeof(ota_eeprom_info_t));
    write_info.magic          = OTA_EE_MAGIC;
    write_info.struct_version = OTA_EE_STRUCT_VERSION;
    write_info.struct_size    = OTA_EE_STRUCT_SIZE;
    write_info.seq            = current_max_seq + 1U;
    write_info.struct_crc32   = ota_eeprom_calc_struct_crc(&write_info);

    /* 执行写入 */
    ret = eeprom_write(OTA_EEPROM_DEVICE_NAME, target_addr, (const uint8_t *)&write_info, sizeof(ota_eeprom_info_t));
    if (ret != (int)sizeof(ota_eeprom_info_t))
    {
        return RET_FAIL;
    }

    return RET_OK;
}


void ota_eeprom_init_default(ota_eeprom_info_t *info)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(ota_eeprom_info_t));

    info->magic          = OTA_EE_MAGIC;
    info->struct_version = OTA_EE_STRUCT_VERSION;
    info->struct_size    = OTA_EE_STRUCT_SIZE;
    info->seq            = 0U;
    info->ota_state      = (uint32_t)OTA_STATE_IDLE;
    info->app_valid      = 1U;  /* 默认认为出厂 App 有效 */
    info->app_confirmed  = 1U;  /* 出厂 App 已确认 */
}


uint32_t ota_eeprom_calc_struct_crc(const ota_eeprom_info_t *info)
{
    /*
     * 对 struct_crc32 字段之前的所有字节计算 CRC32。
     * struct_crc32 是结构体最后一个字段，取它的偏移量作为计算长度。
     */
    size_t len = offset_of(ota_eeprom_info_t, struct_crc32);

    return utils_calculate_crc32((const uint8_t *)info, (uint32_t)len);
}
