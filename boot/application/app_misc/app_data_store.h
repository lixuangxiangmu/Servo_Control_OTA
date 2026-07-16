#ifndef __APP_DATA_STORE_H__
#define __APP_DATA_STORE_H__

#include <stdint.h>

typedef enum
{
    E_PAGE_SIZE512  = 128, /* 24LC512的页写值 */
    E_PAGE_SIZE256  = 64 , /* 24LC256的页写值 */
    E_PAGE_SIZE32   = 32 , /* 24LC32的页写值 */
    E_PAGE_SIZE8    = 8  , /* 24LC02的页写值 */
    E_PAGE_SIZE     = E_PAGE_SIZE8,
} ENUM_EEPROM_PAGE_SIZE;


//得到m在结构体s中得到位置
#define offset_of(t, memb)  ((char*)&(((t*)0)->memb) -(char*)0)

//申明存储联合体定义：t-数据类型，n-成员名，
#define store_allocate( t, n )                                              \
union                                                                       \
{                                                                           \
    t n;                                                                    \
    uint8_t n##_buf[((sizeof(t)+E_PAGE_SIZE-1)/E_PAGE_SIZE)*E_PAGE_SIZE];   \
}


typedef struct
{
    uint8_t servo_speed;                //舵机速度
    uint8_t threshold_angle_fold;       //舵机折叠角度
    uint8_t threshold_angle_init;       //舵机初始角度
    uint8_t enter_lock_sec;             //按键上锁按下时间
    uint8_t enter_unlock_sec;           //按键解锁按下时间
    uint8_t power_off_return;           //断电是否自动回位   0(默认): 自动回位   1: 不回位
    uint8_t power_off_fold_state;       //记录掉电舵机状态   1：折叠状态    0：正常状态
    uint8_t memory_function_flag;       //记忆功能是否开启   1：开启  0：不开启。开启后下次上电时会读取上次掉电时舵机状态，恢复上次状态
    uint8_t unlock_mode;                //历史解锁模式参数，保留 EEPROM 数据兼容
    uint8_t remote_key_code[2];         //遥控器特征码，低字节在前
    uint8_t rcv[3];                     //保留
    uint8_t crc16[2];                   //每次设置参数后都要重新计算CRC，上电时会判断当前EE中的值是否可用
} st_eeprom_servo_data;


/* =====================================================================
 * OTA EEPROM 数据结构
 * ===================================================================== */

/* OTA 升级状态枚举 */
typedef enum
{
    OTA_STATE_IDLE       = 0,   /* 无升级任务 */
    OTA_STATE_REQUEST    = 1,   /* App 请求进入 OTA，等待复位后 Boot 接管 */
    OTA_STATE_DOWNLOADING = 2,  /* Boot 正在接收固件数据 */
    OTA_STATE_VERIFYING  = 3,   /* 固件接收完成，正在进行整体校验 */
    OTA_STATE_SUCCESS    = 4,   /* 升级成功，固件校验通过 */
    OTA_STATE_FAILED     = 5,   /* 升级失败 */
    OTA_STATE_APP_VALID  = 6,   /* App 已被标记为有效 */
    OTA_STATE_APP_PENDING = 7,  /* 新 App 等待启动确认 */
} ota_state_t;

/*
 * EEPROM 中存储的 OTA 运行时信息结构体。
 * 设计为恰好 64 字节，与 BOOT_OTA_METADATA_SIZE 一致。
 * app_base / app_max_size / product_id / hw_id 作为编译期常量
 * 从 app_mem_map.h 和 app_config.h 获取，不重复存储。
 */
typedef struct
{
    uint32_t magic;              /* OTA_EE_MAGIC */
    uint16_t struct_version;     /* 结构体版本号 */
    uint16_t struct_size;        /* 结构体字节大小 (64) */

    uint32_t seq;                /* 双备份版本号，越大越新，A/B 仲裁用 */
    uint32_t ota_state;          /* ota_state_t 枚举值 */

    uint32_t image_size;         /* 固件总大小 (bytes) */
    uint32_t image_crc32;        /* 完整固件 CRC32 */
    uint32_t received_offset;    /* 已可靠写入 Flash 的偏移量 */

    uint32_t transfer_id;        /* 本次升级会话 ID，防止跨复位误匹配 */
    uint32_t target_fw_version;  /* 目标固件版本号 */
    uint32_t fail_reason;        /* 失败原因码 */
    uint32_t retry_count;        /* 当前升级重试次数 */

    uint32_t app_valid;          /* App 是否有效：0=无效，1=有效 */
    uint32_t app_confirmed;      /* 新 App 是否启动确认：0=待确认，1=已确认 */

    uint32_t reserved[1];        /* 保留字段，后续扩展用 */

    uint32_t struct_crc32;       /* 对 struct_crc32 之前所有字段计算的 CRC32 */
} ota_eeprom_info_t;


typedef struct
{
    ota_eeprom_info_t ota_main_data;   /* OTA 主副本 */
    ota_eeprom_info_t ota_back_data;   /* OTA 备份副本 */
} st_eeprom_ota_data;


//EEPROM总表
typedef struct
{
    store_allocate( st_eeprom_servo_data, servo_data );
    store_allocate( st_eeprom_ota_data,    ota_data );
} st_eeprom_allocate;


#endif
