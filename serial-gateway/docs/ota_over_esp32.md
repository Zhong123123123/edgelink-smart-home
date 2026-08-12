# STM32 OTA over ESP32 WiFi Adapter

## 1. 架构

```text
Linux OTA Manager
  -> TCP Binary
  -> ESP32 Comm Adapter
  -> UART
  -> STM32 Bootloader
```

- ESP32 角色：AA55 帧感知转发器。
- ESP32 不解析 `PREPARE/DATA/VERIFY/COMMIT` 业务含义。
- ESP32 不生成 OTA ACK/NACK。
- OTA ACK/NACK、CRC32、seq/offset、rollback 由 Linux 与 STM32 Bootloader 端到端处理。

## 2. 运行态到升级态流程

1. Linux 下发 `reboot_to_bootloader` 给 STM32 APP。  
2. STM32 APP 返回 command ACK，写 RTC backup magic，系统复位。  
3. Bootloader 检测 OTA request magic，进入 OTA 等待态。  
4. Bootloader 发送 `BOOT_HELLO`。  
5. Linux OTA Manager 开始 `PREPARE -> DATA -> VERIFY -> COMMIT`。  
6. Bootloader COMMIT 成功后复位进入新 APP。  
7. 新 APP 启动并执行 `APP_CONFIRM`，metadata 进入 confirmed 稳定态。  

## 3. Linux OTA Transport 选择

- 支持：
  - `serial`
  - `tcp_binary`
- 可显式指定：

```bash
curl -X POST http://127.0.0.1:9010/api/ota/tasks \
  -H 'Content-Type: application/json' \
  -d '{"device_id":1,"device_type":"stm32f407-smarthome","firmware_id":"stm32f407-smarthome-1.1.0","transport":"tcp_binary"}'
```

- 未指定时默认按 device active transport 选择（`tcp_binary` 优先于 `serial`）。

## 4. ESP32 OTA Mode

- Bridge mode：
  - `NORMAL`
  - `OTA`
- 进入/退出通过 Adapter Control Frame（`TYPE=0x7D`）：
  - `ENTER_OTA_MODE`
  - `LEAVE_OTA_MODE`
- OTA mode 策略：
  - 禁止 `drop_oldest`
  - 队列满：直接 close tcp
  - tcp 写失败：直接 close tcp
  - 超时：自动回 NORMAL 并断开 tcp
- Heartbeat（`0x7E`）保持发送，不注入 STM32 UART 业务流。

## 5. 失败场景与行为

- WiFi 断开 / TCP 半开：会话断开，OTA task 进入 `FAILED` 或 `CANCELED`，不误报成功。  
- DATA NACK：按现有 retry 规则处理，耗尽后失败。  
- VERIFY NACK：直接失败，不进入 COMMIT。  
- COMMIT 前掉电：不应误切 active/confirmed，依赖 pending + rollback 规则自恢复。  
- APP 未 confirm：由 Bootloader `boot_attempts + rollback` 保护。  

## 6. Bootloader 等待超时

- Bootloader OTA loop 支持空闲总超时（`BL_OTA_IDLE_TIMEOUT_MS`）。
- 在“RTC magic 强制进入 Bootloader”路径下，超时后回主流程，按现有 metadata 规则跳回当前可启动 APP。
- 在“无可启动槽兜底留在 Bootloader”路径下，保持无限等待 OTA。

## 7. 使用与验证

### 7.1 构建

```bash
cmake -S . -B build
cmake --build build -j
```

### 7.2 Mock 验证（必做）

```bash
tools/integration/test_ota_over_tcp_binary.sh
tools/integration/test_ota_over_tcp_binary_fault_matrix.sh
```

### 7.3 实物验证（建议流程）

1. 启动 Linux gateway（含 tcp_binary）。  
2. 启动 ESP32 adapter 并连接 gateway。  
3. STM32 运行态执行 `reboot_to_bootloader`。  
4. 触发 OTA（`transport=tcp_binary`）。  
5. 验证 A->B 成功，APP_CONFIRM 生效。  
6. 再验证 B->A 成功。  

## 8. 相关文档

- `docs/stm32_ota_contract.md`
- `docs/ota_over_esp32_test_report.md`
- `docs/stm32_esp32_comm_adapter.md`
