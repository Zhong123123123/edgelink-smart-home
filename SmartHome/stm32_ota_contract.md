# STM32 OTA 联调契约（Gateway <-> Bootloader <-> APP）

本文用于统一 STM32 OTA 联调口径，覆盖包格式、分片/重传、状态时序、回滚规则与错误码。

## 1. 基础信息

- 传输链路：TCP -> `stm32_ota_bridge` -> USART3（PB10/PB11, 115200 8N1）
- 帧头：`0xAA 0x55`
- `LEN`：1 byte，表示 `TYPE + PAYLOAD` 长度
- CRC16：Modbus，计算区间 `LEN + TYPE + PAYLOAD`，小端输出

## 2. 分区与槽位

- Bootloader：`0x08000000`，64KB（sectors 0-3）
- Metadata：`0x08010000`，64KB（sector 4）
- APP_A：`0x08020000`，384KB（sectors 5-7）
- APP_B：`0x08080000`，384KB（sectors 8-10）
- CrashLog：`0x080E0000`，128KB（sector 11）

槽位定义：
- `BL_SLOT_A = 0`
- `BL_SLOT_B = 1`
- `BL_SLOT_NONE = 0xFF`

## 3. 命令与响应

下行（Gateway -> Bootloader）：
- `0x31 OTA_PREPARE`
- `0x32 OTA_DATA`
- `0x33 OTA_VERIFY`
- `0x34 OTA_COMMIT`
- `0x35 OTA_ABORT`
- `0x37 GET_VERSION`

上行（Bootloader -> Gateway）：
- `0x20 BOOT_HELLO`
- `0x22 OTA_ACK`，payload: `seq(u16)`
- `0x23 OTA_NACK`，payload: `seq(u16) + error_code(u16)`
- `0x21 VERSION_REPORT`（当前实现返回 A/B version）

## 4. Payload 约定

### 4.1 OTA_PREPARE（兼容旧版）

基础字段（12 bytes）：
- `seq(u16)`
- `image_size(u32)`
- `image_crc32(u32)`
- `chunk_size(u16)`

可选字段：
- `target_base(u32)`：总长 >= 16 时生效
- `image_version(u32)`：总长 >= 20 时生效

Bootloader 行为：
- `chunk_size` 必须 `<= 246`
- `image_size` 必须 `>0 且 <= 384KB`
- 未带 `target_base`：自动选 inactive slot
- 带 `target_base`：仅允许 `0x08020000` 或 `0x08080000`
- 擦除目标 slot 成功后返回 ACK

### 4.2 OTA_DATA

- `seq(u16)`
- `offset(u32)`
- `chunk_len(u16)`
- `chunk_bytes`

约束：
- `chunk_len` 必须等于 `payload_len - 8`
- `offset` 必须严格递增且连续
- `offset + chunk_len <= image_size`
- 每帧成功即 ACK，失败 NACK

### 4.3 OTA_VERIFY

- `seq(u16)`
- `image_crc32(u32)`

Bootloader 校验：
- 目标 slot 前 `image_size` 字节 CRC32 必须匹配
- 向量表校验：
  - SP 在 `0x20000000 ~ 0x20030000`
  - Reset_Handler 在目标 slot 地址范围内

### 4.4 OTA_COMMIT

- `seq(u16)`

Bootloader 行为：
- metadata 标记为 pending
- `pending_slot = target_slot`
- `target_slot_state = IMG_PENDING`
- 写入 `size/crc/version`
- `boot_attempts = 0`
- ACK 后复位

## 5. 分片与重传策略（Gateway）

- 默认 `chunk_size = 240`
- 运行时强制 `chunk_size = min(config, 246)`
- 单帧超时：`timeout_ms`（当前默认 1000ms）
- 最大重传次数：`max_retries`（当前默认 3）
- 任一帧重传耗尽则任务失败

## 6. COMMIT / CONFIRM 时序

1. Gateway 完成 DATA + VERIFY 后发送 COMMIT  
2. Bootloader ACK COMMIT 并复位  
3. Bootloader 按 pending 状态机尝试启动新槽  
4. APP 启动后约 15s 执行本地确认并上报 `APP_CONFIRM(0x26)`  
5. 本地确认成功时 metadata 更新：
- `active_slot = current_slot`
- `confirmed_slot = current_slot`
- `pending_slot = BL_SLOT_NONE`
- `boot_attempts = 0`
- `current_slot_state = IMG_CONFIRMED`

## 7. 回滚触发条件

满足任一条件触发回滚逻辑：
- pending 镜像 CRC32 校验失败
- pending 镜像向量表非法
- `boot_attempts >= BL_MAX_BOOT_ATTEMPTS`

回滚动作：
- pending slot 标记 `IMG_ROLLBACK`
- `pending_slot = BL_SLOT_NONE`
- `rollback_count++`
- 回退到 confirmed slot 启动

## 8. Bootloader 强制进入机制

- APP 收到 `GW_CMD_REBOOT_TO_BOOTLOADER`：
  - 先回 ACK
  - 写 RTC BKP0R magic：`0xB00710AD`
  - `NVIC_SystemReset()`
- Bootloader 上电读取 BKP0R：
  - 命中 magic -> 清零并进入 OTA loop
  - 否则按 metadata 状态机正常跳 APP

## 9. NACK error_code 对齐（当前实现）

- `1`：bad payload
- `2`：bad chunk_size
- `3`：bad target
- `4`：erase failed
- `5`：bad offset / seq
- `6`：flash write failed
- `7`：crc32 mismatch
- `8`：vector invalid
- `9`：metadata commit failed

建议网关在任务事件中直接映射以上错误码，避免仅输出 `ack timeout/ack bad len`。

## 10. 联调验收最小标准

一次完整 OTA 验收应满足：
- 从 A 升 B 成功（任务 `SUCCESS`）
- 设备复位后运行 B（向量表地址位于 `0x08080000` 槽）
- APP 发出 `APP_CONFIRM`
- metadata 最终为 `active=B, confirmed=B, pending=NONE`
- 再从 B 升 A 成功（双向闭环）
