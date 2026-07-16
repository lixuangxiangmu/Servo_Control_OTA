# GD32F103RCT6 舵机控制器 OTA 升级功能设计方案

> 适用对象：GD32F103RCT6 舵机控制器、IAR 工程、Boot + App 双工程架构、蓝牙模块串口通信、微信小程序下发升级包、板载 EEPROM 保存升级状态。  
> 文档目标：在当前项目可落地，同时协议和流程尽量通用，便于后续复用到其他 MCU 项目。

---

## 1. 设计目标

### 1.1 当前项目目标

1. 通过微信小程序选择或上传升级包 `bin` 文件。
2. 微信小程序通过蓝牙模块与 MCU 串口通信，将升级包分包发送给 MCU。
3. Bootloader 接收升级数据，写入 App 区 Flash。
4. 升级过程中支持掉电保护、断点续传、重复包处理、CRC 校验。
5. 升级完成后 Bootloader 校验 App 完整性，校验通过后跳转 App。
6. EEPROM 只保存升级状态、版本信息、进度、校验值等小数据，不用于保存完整固件。

### 1.2 通用协议目标

这套协议不只服务当前舵机控制器，也要方便以后用于其他 MCU 项目，因此协议设计应满足：

1. 与具体芯片解耦：GD32、STM32、HC32、NXP、国产 MCU 均可复用。
2. 与具体传输链路解耦：蓝牙串口、4G TCP、USB CDC、RS485、WiFi TCP 均可复用。
3. 支持命令应答、错误码、超时重发、断点续传。
4. 支持固件信息查询、版本检查、设备信息查询、升级状态查询。
5. 支持后续扩展加密、签名、压缩、差分升级。

---

## 2. 总体设计结论

### 2.1 推荐升级方式

当前最推荐采用：

```text
微信小程序
   ↓ 蓝牙
蓝牙模块
   ↓ UART
Bootloader
   ↓ 写 Flash
App 区
```

即：**App 只负责接收“进入升级模式”的命令，然后写 EEPROM 标志并复位；真正的固件接收、擦除、写入、校验全部由 Bootloader 完成。**

### 2.2 为什么推荐 Bootloader 直接接收升级包

不建议让 App 一边运行一边擦写自身 App 区，原因如下：

1. App 正在运行时擦写 App 区风险很高，容易异常或 HardFault。
2. 如果升级中途断电，App 区可能已经损坏，必须由 Bootloader 保证还能继续升级。
3. EEPROM 容量通常较小，不能保存完整 bin，只适合保存升级状态和进度。
4. 当前 Boot 区 32 KB，建议将最小串口协议、Flash 写入、CRC 校验、EEPROM 状态管理全部放到 Boot 中。

因此当前项目建议做成：

```text
App 收到 OTA 请求
   ↓
App 写 EEPROM: OTA_REQUEST
   ↓
App 软件复位
   ↓
Boot 启动后发现 OTA_REQUEST
   ↓
Boot 进入 OTA 接收模式
   ↓
微信小程序重新连接蓝牙并继续下发 bin
   ↓
Boot 写入 App 区
   ↓
Boot 校验成功
   ↓
Boot 写 EEPROM: APP_VALID
   ↓
Boot 跳转 App
```

---

## 3. 存储空间规划

### 3.1 当前项目分区

根据你当前规划：

| 区域 | 起始地址 | 大小 | 说明 |
|---|---:|---:|---|
| Boot | `0x08000000` | 32 KB | Bootloader 程序区，不允许 OTA 覆盖 |
| App | `0x08008000` | 128 KB | 主应用程序区，OTA 写入目标区 |
| EEPROM | 外部/板载 EEPROM | 按实际容量 | 保存 OTA 状态、升级进度、版本、CRC 等 |

### 3.2 是否需要临时区 OTA_TEMP

如果芯片内部 Flash 总容量足够，并且剩余空间可以容纳完整 App，则可以增加 `OTA_TEMP` 临时区，实现更安全的 A/B 升级：

```text
Boot + App_A + OTA_TEMP
```

但你当前 App 区规划 128 KB，如果剩余 Flash 不足以再放一个完整 128 KB 固件，则不建议强行做 A/B。当前项目优先采用 **Boot 直接写 App 区**。

### 3.3 Boot 直接写 App 区的风险和解决办法

风险：升级过程中 App 区可能被擦除或写坏。  
解决办法：Boot 区必须永远保持可运行，并具备完整 OTA 接收能力。

因此必须满足：

1. Boot 不能依赖 App 的任何函数。
2. Boot 中必须包含 UART 驱动、蓝牙模块基础通信、协议解析、Flash 擦写、EEPROM 读写、CRC 校验。
3. 只要 Boot 区不损坏，即使 App 区损坏，也可以重新进入 OTA 模式恢复。

---

## 4. Boot 和 App 职责划分

### 4.1 Bootloader 职责

Bootloader 必须实现：

1. 上电后读取 EEPROM OTA 状态。
2. 判断是否需要进入 OTA 模式。
3. 校验 App 是否有效。
4. 初始化最小硬件环境：时钟、UART、蓝牙模块、Flash、EEPROM、CRC、看门狗。
5. 接收微信小程序下发的升级数据。
6. 擦除、写入、回读校验 App 区 Flash。
7. 保存升级进度到 EEPROM。
8. 支持断点续传。
9. 校验完整固件 CRC32。
10. 跳转 App。

### 4.2 App 职责

App 只需要实现：

1. 正常业务功能。
2. 与微信小程序通信。
3. 支持查询当前固件版本、硬件版本、产品型号。
4. 收到 `ENTER_OTA` 命令后，写 EEPROM OTA 请求标志。
5. 软件复位进入 Bootloader。
6. App 首次启动后向 EEPROM 写入 `APP_CONFIRMED`，表示新 App 自检成功。

### 4.3 微信小程序职责

微信小程序需要实现：

1. 选择或上传升级包 bin 文件。
2. 读取 bin 文件大小、版本、CRC32。
3. 连接蓝牙设备。
4. 查询设备信息。
5. 判断固件是否匹配当前产品和硬件版本。
6. 发送进入 OTA 命令。
7. 设备复位后重新连接蓝牙。
8. 查询 Boot 当前升级状态和断点位置。
9. 按协议分包发送固件数据。
10. 显示升级进度、速度、错误原因。
11. 升级完成后发送结束命令并等待设备重启。

---

## 5. EEPROM OTA 状态设计

### 5.1 EEPROM 保存内容

EEPROM 不保存完整固件，只保存以下小数据：

1. OTA 状态。
2. 当前升级包大小。
3. 固件 CRC32。
4. 已接收偏移地址。
5. 当前固件版本。
6. 目标固件版本。
7. 升级会话 ID。
8. App 是否有效。
9. 新 App 是否已经启动确认。
10. EEPROM 结构体自身 CRC。

### 5.2 EEPROM 双备份设计

建议 EEPROM 中保存两份 OTA 信息，防止写 EEPROM 时掉电导致状态损坏：

```text
EEPROM
├── OTA_INFO_COPY_A
└── OTA_INFO_COPY_B
```

每次写入时轮流更新 A/B 两份，读取时选择：

1. magic 正确；
2. struct_crc 正确；
3. seq 最大；
4. 状态合法。

### 5.3 OTA 状态枚举

```c
typedef enum
{
    OTA_STATE_IDLE = 0,          /* 无升级任务 */
    OTA_STATE_REQUEST = 1,       /* App 请求进入 OTA */
    OTA_STATE_DOWNLOADING = 2,   /* Boot 正在接收固件 */
    OTA_STATE_VERIFYING = 3,     /* 固件接收完成，正在校验 */
    OTA_STATE_SUCCESS = 4,       /* 升级成功 */
    OTA_STATE_FAILED = 5,        /* 升级失败 */
    OTA_STATE_APP_VALID = 6,     /* App 有效 */
    OTA_STATE_APP_PENDING = 7,   /* 新 App 待确认 */
} ota_state_t;
```

### 5.4 EEPROM 结构体建议

```c
#define OTA_EE_MAGIC        0x4F544131UL  /* "OTA1" */
#define OTA_EE_VERSION      0x0001

typedef struct
{
    uint32_t magic;
    uint16_t struct_version;
    uint16_t struct_size;

    uint32_t seq;                 /* 双备份版本号，越大越新 */
    uint32_t ota_state;

    uint32_t app_base;
    uint32_t app_max_size;

    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t received_offset;      /* 已经可靠写入 Flash 的偏移 */

    uint32_t transfer_id;          /* 本次升级会话 ID */

    uint32_t current_fw_version;
    uint32_t target_fw_version;
    uint32_t product_id;
    uint32_t hw_id;

    uint32_t app_valid;            /* 0: 无效，1: 有效 */
    uint32_t app_confirmed;        /* 0: 待确认，1: 已确认 */

    uint32_t fail_reason;
    uint32_t retry_count;

    uint32_t reserved[8];

    uint32_t struct_crc32;         /* 对 struct_crc32 之前的数据计算 CRC32 */
} ota_eeprom_info_t;
```

### 5.5 EEPROM 写入策略

为了减少 EEPROM 磨损，不建议每收到一包数据就写 EEPROM。建议：

1. 每写完一个 Flash 页后更新一次 `received_offset`。
2. 收到 `OTA_START` 时更新一次。
3. 收到 `OTA_END` 校验成功后更新一次。
4. 发生严重错误时更新一次。
5. 每次写入使用 A/B 双备份，带 `seq` 和 `struct_crc32`。

---

## 6. 固件 bin 文件设计

### 6.1 App bin 的基本要求

App bin 必须从 `APP_BASE_ADDR = 0x08008000` 开始链接。

App 的中断向量表必须位于 App 起始地址：

```text
0x08008000: 初始 MSP
0x08008004: Reset_Handler
0x08008008: NMI_Handler
0x0800800C: HardFault_Handler
...
```

Boot 跳转 App 前会检查：

1. App 首地址处的 MSP 是否位于 SRAM 范围。
2. App Reset_Handler 是否位于 App Flash 范围。
3. App 固件大小是否不超过 `APP_MAX_SIZE`。
4. App CRC32 是否正确。

---

## 7. Bootloader 启动流程

### 7.1 启动判断逻辑

Boot 每次上电或复位后执行：

```text
Boot_Reset_Handler
    ↓
初始化最小硬件
    ↓
读取 EEPROM OTA 信息
    ↓
EEPROM 信息无效？
    ├── 是：校验 App
    └── 否：继续判断 OTA 状态
    ↓
OTA_STATE_REQUEST / DOWNLOADING / FAILED 且 App 无效？
    ├── 是：进入 OTA 模式
    └── 否：校验 App
    ↓
App 校验通过？
    ├── 是：跳转 App
    └── 否：进入 OTA 模式
```

### 7.2 Boot 状态机

```text
BOOT_INIT
   ↓
BOOT_LOAD_OTA_INFO
   ↓
BOOT_CHECK_REQUEST
   ├── 有 OTA 请求 → BOOT_OTA_WAIT_CONNECT
   └── 无 OTA 请求 → BOOT_CHECK_APP
                         ├── App 有效 → BOOT_JUMP_APP
                         └── App 无效 → BOOT_OTA_WAIT_CONNECT

BOOT_OTA_WAIT_CONNECT
   ↓
BOOT_OTA_HANDSHAKE
   ↓
BOOT_OTA_START
   ↓
BOOT_OTA_RECEIVE_DATA
   ↓
BOOT_OTA_VERIFY
   ├── 成功 → BOOT_OTA_SUCCESS → BOOT_JUMP_APP
   └── 失败 → BOOT_OTA_FAILED → BOOT_OTA_WAIT_CONNECT
```

### 7.3 Boot 等待 OTA 超时策略

建议分两种情况：

1. **App 有效，并且只是普通上电：** Boot 直接跳转 App。
2. **App 无效或 EEPROM 标志为 OTA_REQUEST / DOWNLOADING：** Boot 不跳 App，一直等待 OTA。

这样可以防止升级中断后设备变砖。

---

## 8. App 进入 OTA 流程

### 8.1 App 收到 OTA 请求

App 正常运行时，微信小程序发送 `ENTER_OTA` 命令。

App 收到后执行：

```c
void app_enter_ota(void)
{
    ota_eeprom_info_t info;

    ota_eeprom_load(&info);

    info.ota_state = OTA_STATE_REQUEST;
    info.app_valid = 1;
    info.app_confirmed = 1;
    info.received_offset = 0;
    info.image_size = 0;
    info.image_crc32 = 0;
    info.transfer_id = generate_transfer_id();
    info.fail_reason = 0;

    ota_eeprom_save(&info);

    delay_ms(100);
    system_reset();
}
```

### 8.2 App 首次启动确认

升级成功后，Boot 可以先将 EEPROM 标记为：

```text
OTA_STATE_APP_PENDING
```

App 启动后完成基础自检，例如：

1. 时钟正常。
2. 参数区读取正常。
3. 主要任务初始化正常。
4. 蓝牙通信初始化正常。
5. 舵机控制逻辑没有严重错误。

然后写：

```text
app_confirmed = 1
ota_state = OTA_STATE_APP_VALID
```

如果当前项目没有备份旧 App，则 `APP_PENDING` 主要用于记录状态和诊断；如果以后做 A/B 升级，则可以用于失败回滚。

---

## 9. 通用 OTA 通讯协议设计

### 9.1 协议基本原则

1. 小端格式。
2. 一问一答，所有关键命令必须有响应。
3. 每帧有帧 CRC，防止串口误码。
4. 每个数据包有数据 CRC，防止写入错误。
5. 整个固件有 Image CRC32，防止整体文件损坏。
6. 支持 `seq` 序号，便于超时重发和重复包识别。
7. 支持 `session_id`，防止设备复位后误接收旧数据。
8. 支持 `offset`，便于断点续传。
9. 不依赖具体传输介质。

### 9.2 帧格式

建议所有命令统一使用如下二进制帧格式：

```text
+----------+----------+-------------+-------------+-------------+
| 字段     | 长度     | 示例        | 说明        | 备注        |
+----------+----------+-------------+-------------+-------------+
| SOF      | 2 bytes  | 55 AA       | 帧头        | 固定值      |
| Ver      | 1 byte   | 01          | 协议版本    | 当前为 1    |
| Type     | 1 byte   | 01          | 帧类型      | 请求/响应   |
| Flags    | 1 byte   | 00          | 标志位      | 加密/分片等 |
| HLen     | 1 byte   | 14          | 头长度      | 固定 20     |
| Seq      | 2 bytes  | 00 01       | 当前帧序号  | 小端        |
| AckSeq   | 2 bytes  | 00 00       | 应答序号    | 小端        |
| Cmd      | 2 bytes  | 10 00       | 命令字      | 小端        |
| Len      | 2 bytes  | xx xx       | Payload长度 | 小端        |
| Session  | 4 bytes  | xx xx xx xx | 会话 ID     | 小端        |
| HCRC16   | 2 bytes  | xx xx       | 头 CRC      | 可选校验    |
| Payload  | N bytes  | ...         | 数据        | N = Len     |
| FCRC16   | 2 bytes  | xx xx       | 整帧 CRC    | 推荐必选    |
+----------+----------+-------------+-------------+-------------+
```

其中：

```c
#define OTA_SOF0              0x55
#define OTA_SOF1              0xAA
#define OTA_PROTO_VER         0x01
#define OTA_HEADER_LEN        20
#define OTA_MAX_PAYLOAD       512
```

CRC 建议：

1. `HCRC16`：对 `Ver` 到 `Session` 字段计算 CRC16，计算时 `HCRC16` 字段不参与。
2. `FCRC16`：对 `Ver` 到 `Payload` 末尾计算 CRC16，不包含 `SOF` 和 `FCRC16` 自身。
3. 完整固件使用 CRC32。

### 9.3 帧类型

```c
typedef enum
{
    FRAME_TYPE_REQ   = 0x01,
    FRAME_TYPE_RSP   = 0x02,
    FRAME_TYPE_ACK   = 0x03,
    FRAME_TYPE_NACK  = 0x04,
    FRAME_TYPE_EVENT = 0x05,
} frame_type_t;
```

### 9.4 命令字设计

```c
typedef enum
{
    CMD_PING             = 0x0001,
    CMD_GET_INFO         = 0x0002,
    CMD_GET_OTA_STATUS   = 0x0003,
    CMD_ENTER_OTA        = 0x0004,
    CMD_REBOOT           = 0x0005,

    CMD_OTA_START        = 0x0010,
    CMD_OTA_DATA         = 0x0011,
    CMD_OTA_END          = 0x0012,
    CMD_OTA_ABORT        = 0x0013,
    CMD_OTA_VERIFY       = 0x0014,
    CMD_OTA_RESUME       = 0x0015,

    CMD_READ_PARAM       = 0x0020,
    CMD_WRITE_PARAM      = 0x0021,

    CMD_VENDOR_BEGIN     = 0x8000,
} ota_cmd_t;
```

### 9.5 通用错误码

```c
typedef enum
{
    OTA_OK                       = 0x0000,
    OTA_ERR_UNKNOWN_CMD          = 0x0001,
    OTA_ERR_FRAME_CRC            = 0x0002,
    OTA_ERR_PAYLOAD_LEN          = 0x0003,
    OTA_ERR_BUSY                 = 0x0004,
    OTA_ERR_NOT_IN_OTA_MODE      = 0x0005,

    OTA_ERR_PRODUCT_MISMATCH     = 0x0100,
    OTA_ERR_HW_MISMATCH          = 0x0101,
    OTA_ERR_VERSION_TOO_OLD      = 0x0102,
    OTA_ERR_IMAGE_TOO_LARGE      = 0x0103,
    OTA_ERR_IMAGE_CRC            = 0x0104,
    OTA_ERR_IMAGE_INVALID        = 0x0105,

    OTA_ERR_OFFSET_MISMATCH      = 0x0200,
    OTA_ERR_SEQ_MISMATCH         = 0x0201,
    OTA_ERR_CHUNK_CRC            = 0x0202,
    OTA_ERR_FLASH_ERASE          = 0x0203,
    OTA_ERR_FLASH_WRITE          = 0x0204,
    OTA_ERR_FLASH_READBACK       = 0x0205,
    OTA_ERR_EEPROM               = 0x0206,

    OTA_ERR_TIMEOUT              = 0x0300,
    OTA_ERR_ABORTED              = 0x0301,
} ota_status_t;
```

---

## 10. 协议 Payload 设计

### 10.1 CMD_GET_INFO

#### 请求 Payload

无。

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t boot_version;
    uint32_t app_version;
    uint32_t app_base;
    uint32_t app_max_size;
    uint32_t flash_page_size;
    uint32_t protocol_version;
    uint32_t support_flags;
    uint8_t  device_sn[16];
} get_info_rsp_t;
```

`support_flags` 建议定义：

```c
#define OTA_SUPPORT_RESUME        (1UL << 0)
#define OTA_SUPPORT_ENCRYPT       (1UL << 1)
#define OTA_SUPPORT_SIGN          (1UL << 2)
#define OTA_SUPPORT_COMPRESS      (1UL << 3)
#define OTA_SUPPORT_AB            (1UL << 4)
```

### 10.2 CMD_ENTER_OTA

#### 请求 Payload

```c
typedef struct
{
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t target_fw_version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t transfer_id;
    uint32_t option_flags;
} enter_ota_req_t;
```

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t transfer_id;
    uint32_t reboot_delay_ms;
} enter_ota_rsp_t;
```

App 收到该命令后：

1. 检查产品型号是否匹配。
2. 检查硬件版本是否匹配。
3. 检查固件大小是否不超过 App 区。
4. 写 EEPROM：`OTA_STATE_REQUEST`。
5. 回复成功。
6. 延时 `reboot_delay_ms` 后复位。

### 10.3 CMD_GET_OTA_STATUS

#### 请求 Payload

无。

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t ota_state;
    uint32_t transfer_id;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t received_offset;
    uint32_t app_valid;
    uint32_t fail_reason;
} get_ota_status_rsp_t;
```

小程序连接 Boot 后，先发送该命令。如果发现 `received_offset` 不为 0，则从该偏移继续发送。

### 10.4 CMD_OTA_START

#### 请求 Payload

```c
typedef struct
{
    uint32_t product_id;
    uint32_t hw_id;
    uint32_t fw_version;
    uint32_t image_size;
    uint32_t image_crc32;
    uint32_t transfer_id;
    uint32_t chunk_size;
    uint32_t option_flags;
} ota_start_req_t;
```

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t transfer_id;
    uint32_t accepted_chunk_size;
    uint32_t next_offset;
    uint32_t erase_policy;
} ota_start_rsp_t;
```

Boot 收到 `OTA_START` 后：

1. 检查是否处于 OTA 模式。
2. 检查产品、硬件、版本、大小、CRC。
3. 如果是新升级任务，擦除 App 区或按页边写边擦。
4. 如果是断点续传，返回 EEPROM 中保存的 `received_offset`。
5. 设置状态为 `OTA_STATE_DOWNLOADING`。

### 10.5 CMD_OTA_DATA

#### 请求 Payload

```c
typedef struct
{
    uint32_t offset;       /* 当前数据写入 App 区的偏移 */
    uint16_t data_len;     /* data 实际长度 */
    uint16_t data_crc16;   /* 仅对 data 计算 */
    uint8_t  data[0];      /* 固件数据 */
} ota_data_req_t;
```

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t received_offset;  /* Boot 已经可靠写入的下一个偏移 */
    uint32_t expect_seq;
} ota_data_rsp_t;
```

Boot 处理逻辑：

1. 检查帧 CRC。
2. 检查 `offset == received_offset`。
3. 检查 `data_crc16`。
4. 必要时擦除当前 Flash 页。
5. 写入 Flash。
6. 回读校验。
7. 更新 RAM 中 `received_offset`。
8. 每完成一个 Flash 页后更新 EEPROM。
9. 回复 `received_offset`。

如果收到重复包：

1. 如果 `offset < received_offset`，说明该包已经写过。
2. Boot 不重复写 Flash。
3. 直接回复当前 `received_offset`。

如果收到跳包：

1. 如果 `offset > received_offset`，说明中间缺包。
2. Boot 回复 `OTA_ERR_OFFSET_MISMATCH`。
3. 响应中带上期望的 `received_offset`。
4. 小程序从该偏移重新发送。

### 10.6 CMD_OTA_END

#### 请求 Payload

```c
typedef struct
{
    uint32_t transfer_id;
    uint32_t image_size;
    uint32_t image_crc32;
} ota_end_req_t;
```

#### 响应 Payload

```c
typedef struct
{
    uint32_t status;
    uint32_t image_crc32_calc;
    uint32_t reboot_delay_ms;
} ota_end_rsp_t;
```

Boot 收到 `OTA_END` 后：

1. 检查 `received_offset == image_size`。
2. 对 App 区计算 CRC32。
3. 校验向量表是否合法。
4. 校验固件信息结构体是否合法。
5. 校验成功后写 EEPROM：`OTA_STATE_SUCCESS`、`app_valid = 1`、`app_confirmed = 0`。
6. 回复成功。
7. 延时后复位或直接跳转 App。

### 10.7 完整报文示例

以下示例用于说明第 9 章帧格式与第 10 章 Payload 如何组合成实际串口字节流。

示例统一假设：

```text
session_id / transfer_id = 0x12345678
product_id               = 0x00001001
hw_id                    = 0x00000001
fw_version               = 0x00020003
image_size               = 0x00001000
image_crc32              = 0x89ABCDEF
chunk_size               = 256
```

CRC 说明：

1. `HCRC16` 按本工程 `utils_calculate_crc16()` 算法计算，覆盖 `Ver` 到 `Session` 字段，不包含 `SOF` 和 `HCRC16` 自身。
2. `FCRC16` 按本工程 `utils_calculate_crc16()` 算法计算，覆盖 `Ver` 到 `Payload` 末尾，不包含 `SOF` 和 `FCRC16` 自身。
3. `CMD_OTA_DATA` 内部的 `data_crc16` 只对固件数据 `data` 计算。
4. 所有多字节字段均为小端格式。

#### 10.7.1 CMD_GET_INFO 请求

Payload 为空：

```text
55 AA 01 01 00 14 01 00 00 00 02 00 00 00 78 56 34 12 3B A3 47 0F
```

字段解释：

```text
SOF      = 55 AA
Ver      = 01
Type     = 01                 /* REQ */
Flags    = 00
HLen     = 14                 /* 20 bytes */
Seq      = 0001
AckSeq   = 0000
Cmd      = 0002               /* CMD_GET_INFO */
Len      = 0000
Session  = 12345678
HCRC16   = A33B
FCRC16   = 0F47
```

#### 10.7.2 CMD_ENTER_OTA 请求

Payload：

```text
product_id         = 0x00001001
hw_id              = 0x00000001
target_fw_version  = 0x00020003
image_size         = 0x00001000
image_crc32        = 0x89ABCDEF
transfer_id        = 0x12345678
option_flags       = 0x00000000
```

完整报文：

```text
55 AA 01 01 00 14 02 00 00 00 04 00 1C 00 78 56 34 12 20 4A
01 10 00 00 01 00 00 00 03 00 02 00 00 10 00 00 EF CD AB 89 78 56 34 12 00 00 00 00
70 5B
```

其中：

```text
Seq      = 0002
Cmd      = 0004               /* CMD_ENTER_OTA */
Len      = 001C               /* 28 bytes */
HCRC16   = 4A20
FCRC16   = 5B70
```

#### 10.7.3 CMD_ENTER_OTA 响应

Payload：

```text
status           = 0x00000000  /* OTA_OK */
transfer_id      = 0x12345678
reboot_delay_ms  = 1000
```

完整报文：

```text
55 AA 01 02 00 14 01 00 02 00 04 00 0C 00 78 56 34 12 4E 0F
00 00 00 00 78 56 34 12 E8 03 00 00
2F ED
```

其中：

```text
Type     = 02                 /* RSP */
Seq      = 0001
AckSeq   = 0002
Cmd      = 0004               /* CMD_ENTER_OTA */
Len      = 000C               /* 12 bytes */
HCRC16   = 0F4E
FCRC16   = ED2F
```

#### 10.7.4 CMD_GET_OTA_STATUS 响应

Payload：

```text
status           = 0x00000000  /* OTA_OK */
ota_state        = 0x00000002  /* OTA_STATE_READY / 示例状态 */
transfer_id      = 0x12345678
image_size       = 0x00001000
image_crc32      = 0x89ABCDEF
received_offset  = 0x00000000
app_valid        = 0x00000000
fail_reason      = 0x00000000
```

完整报文：

```text
55 AA 01 02 00 14 02 00 03 00 03 00 20 00 78 56 34 12 1D 8C
00 00 00 00 02 00 00 00 78 56 34 12 00 10 00 00 EF CD AB 89 00 00 00 00 00 00 00 00 00 00 00 00
D4 49
```

其中：

```text
Type     = 02                 /* RSP */
Seq      = 0002
AckSeq   = 0003
Cmd      = 0003               /* CMD_GET_OTA_STATUS */
Len      = 0020               /* 32 bytes */
HCRC16   = 8C1D
FCRC16   = 49D4
```

#### 10.7.5 CMD_OTA_START 请求

Payload：

```text
product_id    = 0x00001001
hw_id         = 0x00000001
fw_version    = 0x00020003
image_size    = 0x00001000
image_crc32   = 0x89ABCDEF
transfer_id   = 0x12345678
chunk_size    = 256
option_flags  = 0x00000000
```

完整报文：

```text
55 AA 01 01 00 14 04 00 00 00 10 00 20 00 78 56 34 12 6F F6
01 10 00 00 01 00 00 00 03 00 02 00 00 10 00 00 EF CD AB 89 78 56 34 12 00 01 00 00 00 00 00 00
0B 93
```

其中：

```text
Seq      = 0004
Cmd      = 0010               /* CMD_OTA_START */
Len      = 0020               /* 32 bytes */
HCRC16   = F66F
FCRC16   = 930B
```

#### 10.7.6 CMD_OTA_START 响应

Payload：

```text
status               = 0x00000000  /* OTA_OK */
transfer_id          = 0x12345678
accepted_chunk_size  = 256
next_offset          = 0
erase_policy         = 0
```

完整报文：

```text
55 AA 01 02 00 14 03 00 04 00 10 00 14 00 78 56 34 12 69 CB
00 00 00 00 78 56 34 12 00 01 00 00 00 00 00 00 00 00 00 00
38 FB
```

其中：

```text
Type     = 02                 /* RSP */
Seq      = 0003
AckSeq   = 0004
Cmd      = 0010               /* CMD_OTA_START */
Len      = 0014               /* 20 bytes */
HCRC16   = CB69
FCRC16   = FB38
```

#### 10.7.7 CMD_OTA_DATA 请求

示例发送 8 字节固件数据：

```text
data = 01 02 03 04 05 06 07 08
```

Payload：

```text
offset      = 0x00000000
data_len    = 0x0008
data_crc16  = 0x6DD4
data        = 01 02 03 04 05 06 07 08
```

完整报文：

```text
55 AA 01 01 00 14 05 00 00 00 11 00 10 00 78 56 34 12 55 35
00 00 00 00 08 00 D4 6D 01 02 03 04 05 06 07 08
DD 43
```

其中：

```text
Seq      = 0005
Cmd      = 0011               /* CMD_OTA_DATA */
Len      = 0010               /* offset(4) + data_len(2) + data_crc16(2) + data(8) */
HCRC16   = 3555
FCRC16   = 43DD
```

#### 10.7.8 CMD_OTA_DATA 响应

Payload：

```text
status           = 0x00000000  /* OTA_OK */
received_offset  = 0x00000008
expect_seq       = 0x00000006
```

完整报文：

```text
55 AA 01 02 00 14 04 00 05 00 11 00 0C 00 78 56 34 12 81 87
00 00 00 00 08 00 00 00 06 00 00 00
4C CF
```

其中：

```text
Type     = 02                 /* RSP */
Seq      = 0004
AckSeq   = 0005
Cmd      = 0011               /* CMD_OTA_DATA */
Len      = 000C               /* 12 bytes */
HCRC16   = 8781
FCRC16   = CF4C
```

#### 10.7.9 CMD_OTA_END 请求

Payload：

```text
transfer_id   = 0x12345678
image_size    = 0x00001000
image_crc32   = 0x89ABCDEF
```

完整报文：

```text
55 AA 01 01 00 14 06 00 00 00 12 00 0C 00 78 56 34 12 2F 4B
78 56 34 12 00 10 00 00 EF CD AB 89
6D 3D
```

其中：

```text
Seq      = 0006
Cmd      = 0012               /* CMD_OTA_END */
Len      = 000C               /* 12 bytes */
HCRC16   = 4B2F
FCRC16   = 3D6D
```

#### 10.7.10 CMD_OTA_END 响应

Payload：

```text
status            = 0x00000000  /* OTA_OK */
image_crc32_calc  = 0x89ABCDEF
reboot_delay_ms   = 500
```

完整报文：

```text
55 AA 01 02 00 14 05 00 06 00 12 00 0C 00 78 56 34 12 6D FC
00 00 00 00 EF CD AB 89 F4 01 00 00
A2 46
```

其中：

```text
Type     = 02                 /* RSP */
Seq      = 0005
AckSeq   = 0006
Cmd      = 0012               /* CMD_OTA_END */
Len      = 000C               /* 12 bytes */
HCRC16   = FC6D
FCRC16   = 46A2
```

---

## 11. OTA 升级完整交互流程

### 11.1 正常升级流程

```text
微信小程序                       App                         Boot
    |                            |                            |
    | ---- GET_INFO ------------>|                            |
    | <--- INFO_RSP -------------|                            |
    |                            |                            |
    | ---- ENTER_OTA ----------->|                            |
    | <--- ENTER_OTA_RSP --------|                            |
    |                            | 写 EEPROM: OTA_REQUEST      |
    |                            | 软件复位                    |
    |                            |---------------------------> |
    |                            |                            | Boot 启动
    |                            |                            | 读取 EEPROM
    |                            |                            | 进入 OTA 模式
    |                            |                            |
    |                            |                            |
    | ---- GET_OTA_STATUS ------------------------------------>|
    | <--- STATUS_RSP -----------------------------------------|
    | ---- OTA_START ----------------------------------------->|
    | <--- OTA_START_RSP --------------------------------------|
    | ---- OTA_DATA offset=0 --------------------------------->|
    | <--- DATA_RSP next_offset -------------------------------|
    | ---- OTA_DATA offset=n --------------------------------->|
    | <--- DATA_RSP next_offset -------------------------------|
    |                            ...                           |
    | ---- OTA_END ------------------------------------------->|
    | <--- OTA_END_RSP ----------------------------------------|
    |                            |                            | 校验成功
    |                            |                            | 写 EEPROM
    |                            |                            | 复位/跳转 App
```

### 11.2 掉电续传流程

```text
Boot 正在接收 OTA_DATA
    ↓
写完某一 Flash 页
    ↓
EEPROM received_offset = page_end_offset
    ↓
突然掉电
    ↓
重新上电
    ↓
Boot 读取 EEPROM: OTA_STATE_DOWNLOADING
    ↓
Boot 不跳 App，继续进入 OTA 模式
    ↓
小程序重新连接
    ↓
GET_OTA_STATUS
    ↓
Boot 返回 received_offset
    ↓
小程序从 received_offset 继续发送
```

### 11.3 App 无效时的恢复流程

```text
上电
  ↓
Boot 校验 App 失败
  ↓
Boot 进入 OTA 等待模式
  ↓
微信小程序连接设备
  ↓
重新下发完整固件
  ↓
升级成功后恢复
```

---

## 12. Flash 擦写策略

### 12.1 擦除策略

有两种方式：

#### 方式 A：OTA_START 时一次性擦除整个 App 区

优点：逻辑简单。  
缺点：擦除时间长，OTA_START 阶段等待明显；一旦擦除后中断，App 立即无效。

#### 方式 B：边接收边按页擦除

优点：启动快，适合分包升级。  
缺点：代码略复杂，需要管理当前页是否已经擦除。

当前建议：**采用方式 B，按页擦除。**

### 12.2 写入策略

建议：

1. 小程序按固定 chunk 发送，例如 128、240 或 256 字节。
2. Boot 接收后先校验 chunk CRC。
3. 若即将写入新 Flash 页，则先擦除该页。
4. Flash 写入按芯片要求对齐，例如 half-word、word 或 double-word。
5. 不足对齐长度的数据用 `0xFF` 补齐。
6. 写入后回读校验。
7. 只有回读正确才更新 `received_offset`。

### 12.3 断点续传与 Flash 页关系

为了避免 EEPROM 频繁写入，建议 `received_offset` 只记录到已经完整写完并校验通过的 Flash 页末尾。

例如：

```text
Flash page size = 2 KB
已收到 5500 bytes
其中 4096 bytes 已经完整写完两个页
EEPROM received_offset = 4096
```

掉电后从 4096 重新发送，虽然会多传一部分数据，但可靠性更高，EEPROM 写入次数更少。

---

## 13. Boot 跳转 App 设计

### 13.1 App 合法性检查

Boot 跳转前必须检查：

```c
bool boot_is_app_valid(void)
{
    uint32_t app_sp = *(uint32_t *)(APP_BASE_ADDR + 0);
    uint32_t app_reset = *(uint32_t *)(APP_BASE_ADDR + 4);

    if (!is_addr_in_sram(app_sp))
    {
        return false;
    }

    if (!is_addr_in_app_flash(app_reset))
    {
        return false;
    }

    if (!ota_check_app_crc32())
    {
        return false;
    }

    return true;
}
```

### 13.2 跳转代码

```c
typedef void (*app_entry_t)(void);

void boot_jump_to_app(void)
{
    uint32_t app_sp = *(uint32_t *)(APP_BASE_ADDR + 0);
    uint32_t app_reset = *(uint32_t *)(APP_BASE_ADDR + 4);

    __disable_irq();

    /* 关闭 SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    /* 关闭或复位 Boot 中开启的外设中断 */
    boot_deinit_peripherals();

    /* 清除 NVIC pending，具体数量按 MCU 实际中断数量处理 */
    for (uint32_t i = 0; i < 8; i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* 重定位中断向量表 */
    SCB->VTOR = APP_BASE_ADDR;

    /* 设置 App 栈指针 */
    __set_MSP(app_sp);

    /* 跳转 App Reset_Handler */
    ((app_entry_t)app_reset)();
}
```

### 13.3 App 中的向量表重定位

App 启动初期也建议设置：

```c
SCB->VTOR = APP_BASE_ADDR;
```

这样即使 Boot 已经设置，App 自己也能保证中断向量表正确。

---

## 14. 蓝牙串口传输建议

### 14.1 chunk 大小

蓝牙串口链路通常不适合一次发送过大的包，建议初始参数：

```c
#define OTA_DEFAULT_CHUNK_SIZE   128
#define OTA_MAX_CHUNK_SIZE       512
```

实际可以在 `OTA_START_RSP` 中由 Boot 返回 `accepted_chunk_size`，小程序按 Boot 返回值发送。

建议优先测试：

1. 128 bytes：最稳。
2. 240 bytes：兼顾速度和稳定性。
3. 512 bytes：需要蓝牙链路和串口缓存都足够稳定。

### 14.2 超时重发

建议小程序侧策略：

```text
发送一包 OTA_DATA
    ↓
等待 Boot 响应
    ↓
800 ms 内收到 ACK：发送下一包
    ↓
800 ms 未收到 ACK：重发当前包
    ↓
连续 5 次失败：暂停升级，提示用户重试
```

### 14.3 Boot 接收缓冲区

Boot 中建议使用 UART DMA + 环形缓冲区，或者中断接收 + 环形缓冲区。

建议缓冲区：

```c
#define OTA_UART_RX_BUF_SIZE     1024
#define OTA_FRAME_BUF_SIZE       768
```

如果 Boot 空间紧张，可以减小，但必须大于最大帧长度。

---

## 15. 微信小程序端设计

### 15.1 小程序页面建议

OTA 页面建议显示：

1. 当前设备名称。
2. 当前硬件版本。
3. 当前 App 版本。
4. 目标固件版本。
5. 固件大小。
6. 固件 CRC32。
7. 当前升级状态。
8. 升级进度条。
9. 当前速度。
10. 失败原因。
11. 重新连接按钮。
12. 继续升级按钮。

### 15.2 小程序 OTA 流程

```text
选择 bin 文件
    ↓
解析 bin / 读取升级包信息
    ↓
连接蓝牙设备
    ↓
发送 GET_INFO
    ↓
检查 product_id / hw_id / version
    ↓
发送 ENTER_OTA
    ↓
等待设备复位
    ↓
重新扫描并连接蓝牙
    ↓
发送 GET_OTA_STATUS
    ↓
发送 OTA_START
    ↓
从 next_offset 开始发送 OTA_DATA
    ↓
发送 OTA_END
    ↓
等待设备重启
    ↓
重新连接并确认新版本
```

### 15.3 小程序断点续传

小程序端不应该只依赖自己记录的进度，而是以 Boot 返回的 `received_offset` 为准。

原因：

1. 小程序认为发出去了，不代表 MCU 已经写入成功。
2. 蓝牙断开可能导致最后几包丢失。
3. MCU 掉电后 EEPROM 中的进度才是可信进度。

因此每次重新连接后：

```text
小程序发送 GET_OTA_STATUS
Boot 返回 received_offset
小程序从 received_offset 继续读取 bin 并发送
```

---

## 16. 升级包打包工具建议

建议做一个简单 PC 工具，例如 Python 脚本：

输入：

```text
app.bin
product_id
hw_id
fw_version
```

输出：

```text
app_产品型号_硬件版本_版本号_crc32.bin
```

工具功能：

1. 读取 app.bin。
2. 检查 bin 大小是否小于 `APP_MAX_SIZE`。
3. 计算 CRC32。
4. 可选：回填 fw_info 结构体中的 `image_size` 和 `image_crc32`。
5. 输出 manifest JSON，供微信小程序显示。

manifest 示例：

```json
{
  "product_id": 4097,
  "hw_id": 1,
  "fw_version": "1.0.2",
  "image_size": 98564,
  "image_crc32": "0x12345678",
  "app_base": "0x08008000",
  "app_max_size": 131072
}
```

---

## 17. 测试用例

### 17.1 基础功能测试

| 编号 | 测试项 | 预期结果 |
|---|---|---|
| T001 | Boot 正常跳转 App | App 正常启动 |
| T002 | App 收到 ENTER_OTA 后复位 | Boot 进入 OTA 模式 |
| T003 | 小程序发送完整 bin | Boot 写入成功 |
| T004 | OTA_END 后 CRC 正确 | Boot 跳转新 App |
| T005 | 新 App 上报版本 | 版本与升级包一致 |

### 17.2 异常测试

| 编号 | 测试项 | 操作 | 预期结果 |
|---|---|---|---|
| T101 | 固件过大 | 发送超过 App 区大小的 bin | Boot 拒绝升级 |
| T102 | 产品型号错误 | product_id 不匹配 | Boot 返回 PRODUCT_MISMATCH |
| T103 | 硬件版本错误 | hw_id 不匹配 | Boot 返回 HW_MISMATCH |
| T104 | 数据包 CRC 错误 | 修改一包数据 | Boot NACK，要求重发 |
| T105 | offset 跳包 | 故意跳过一包 | Boot 返回期望 offset |
| T106 | 重复包 | 重发已经写过的数据 | Boot 不重复写，返回当前 offset |
| T107 | OTA_END CRC 错误 | 修改 image_crc32 | Boot 判定失败，不跳 App |

### 17.3 掉电测试

| 编号 | 测试项 | 操作 | 预期结果 |
|---|---|---|---|
| T201 | OTA_START 后掉电 | 开始升级后立即断电 | 上电后 Boot 继续 OTA |
| T202 | 擦除页后掉电 | 擦除 App 部分页后断电 | 上电后 Boot 不跳坏 App，等待 OTA |
| T203 | 写入 30% 后掉电 | 断电再上电 | 小程序从 Boot 返回 offset 续传 |
| T204 | 写入 99% 后掉电 | OTA_END 前断电 | 上电后继续传最后部分 |
| T205 | EEPROM 写入中掉电 | 模拟 EEPROM 半写 | Boot 选择另一份有效备份 |

### 17.4 长时间稳定性测试

1. 连续升级 100 次。
2. 每次升级后校验版本号和功能。
3. 升级过程中随机断电 50 次。
4. 蓝牙连接随机断开 50 次。
5. 小程序退出后重新进入继续升级。
6. 不同 chunk 大小测试：128、240、512 bytes。

---

## 17. 可复用协议分层建议

为了后续其他 MCU 项目复用，建议代码分层：

```text
ota_core/
├── ota_protocol.c       /* 升级协议编解码，和芯片无关 */
├── ota_protocol.h
├── ota_image.c          /* 固件信息、镜像校验 */
├── ota_image.h
├── ota_service.c          /* OTA 升级逻辑处理 */
├── ota_service.h
```

### 18.1 和芯片无关的部分

以下代码可以直接复用到其他项目：

1. 协议帧解析。
2. 命令字定义。
3. 错误码定义。
4. CRC16 / CRC32。
5. OTA 状态机框架。
6. 固件信息结构体。
7. 微信小程序协议交互。

### 18.2 和芯片相关的部分

以下代码需要按 MCU 适配：

1. Flash 擦除。
2. Flash 写入。
3. Flash 页大小。
4. SRAM 地址范围判断。
5. 中断向量表重定位。
6. 软件复位。
7. EEPROM 读写。
8. UART 接收发送。

---

## 19. 开发落地步骤

建议按以下顺序开发，风险最低：

### 第一步：整理 Boot/App 跳转

1. 确认 Boot 32 KB 正常运行。
2. 确认 App 链接地址为 `0x08008000`。
3. 确认 Boot 可检查向量表并跳转 App。
4. 确认 App 中 `SCB->VTOR` 设置正确。

### 第二步：实现 EEPROM OTA 状态

1. 定义 `ota_eeprom_info_t`。
2. 实现 A/B 双备份读写。
3. 实现 struct CRC32。
4. App 可写 `OTA_STATE_REQUEST`。
5. Boot 可识别 `OTA_STATE_REQUEST`。

### 第三步：实现 Boot 串口协议

1. 先实现 `PING`。
2. 再实现 `GET_INFO`。
3. 再实现 `GET_OTA_STATUS`。
4. 确认微信小程序可以和 Boot 通信。

### 第四步：实现基础 OTA

1. 实现 `OTA_START`。
2. 实现 `OTA_DATA`。
3. 实现 Flash 写入。
4. 实现回读校验。
5. 实现 `OTA_END`。
6. 实现完整 App CRC32 校验。

### 第五步：实现异常恢复

1. 重复包处理。
2. offset 错误处理。
3. CRC 错误处理。
4. 蓝牙断开恢复。
5. 掉电续传。
6. App 无效时强制 OTA。

### 第六步：实现小程序升级页面

1. 选择 bin。
2. 计算 CRC32。
3. 查询设备信息。
4. 进入 OTA。
5. 自动重连 Boot。
6. 分包发送。
7. 显示进度。
8. 失败后继续。

---

## 20. 后续可扩展方向

1. **A/B 双 App 分区**：Flash 足够时实现真正回滚。
2. **升级包签名**：Boot 内置公钥，防止非法固件。
3. **固件加密**：防止 bin 被直接读取和逆向。
4. **差分升级**：减少蓝牙传输数据量。
5. **压缩升级**：小程序发送压缩包，Boot 解压写入。
6. **多固件升级**：同时升级主控、蓝牙模块、参数区。
7. **多链路复用**：蓝牙、USB、TCP 使用同一 OTA 协议。
8. **PC 调试工具**：先做 Python/Qt OTA 工具，再移植到微信小程序。

---

## 21. 一个重要注意点

当前方案里，Bootloader 是整个 OTA 的生命线。只要 Bootloader 不损坏，App 即使升级失败也能恢复。因此后续开发时必须遵守：

1. OTA 永远不擦写 Boot 区。
2. Boot 不信任 App 区内容。
3. Boot 不信任未校验完成的固件。
4. Boot 不因为 App 无效而死循环复位。
5. Boot 必须能在 App 完全损坏时继续通过蓝牙串口升级。
