# OTA over ESP32 实测报告（草稿）

## 1. 目的与范围
本次验证目标是确认 STM32 OTA 能通过 ESP32 通信适配器链路稳定执行，且不破坏原有 OTA 协议语义与 A/B 安全机制。  
验证范围：

- Linux OTA Manager -> tcp_binary -> ESP32 Adapter -> UART -> STM32 Bootloader
- Bootloader OTA 等待与超时回跳逻辑
- OTA 任务在成功与故障注入场景下的状态机行为
- OTA 期间会话隔离（普通命令不抢占）

---

## 2. 测试环境

### 2.1 硬件
- Linux 网关主机
- ESP32 Comm Adapter
- STM32F407（Bootloader + APP A/B）

### 2.2 软件版本
- serial-gateway：本次 OTA over ESP32 改造版本
- ESP32 适配器固件：`esp32_comm_adapter_merged.ino`（包含 OTA mode/0x7D 控制帧）
- STM32 Bootloader：包含 OTA idle timeout 修复版本

### 2.3 关键配置
- OTA transport：`tcp_binary`
- Bootloader idle timeout：`BL_OTA_IDLE_TIMEOUT_MS = 30000`（或当前实际值）

---

## 3. 架构与约束说明

### 3.1 架构路径
Linux OTA Manager -> TCP Binary -> ESP32 Adapter -> UART -> STM32 Bootloader

### 3.2 角色边界
- ESP32 只做 AA55 完整帧转发，不解析 OTA 业务语义
- ESP32 不生成 OTA ACK/NACK
- ACK/NACK、CRC32、seq/offset、rollback 仍由 Linux 与 STM32 端到端处理

---

## 4. 实施改动概述

### 4.1 Linux 侧
- 新增 OTA transport 选择：`serial` / `tcp_binary`
- 默认按 active transport 选择，支持显式指定 `tcp_binary`
- OTA 期间设备进入 `device_in_ota` 隔离
- 接入 adapter control frame（0x7D）进入/退出 OTA mode
- tcp_binary OTA 回流 ACK/NACK 支持

### 4.2 ESP32 侧
- 新增 bridge mode：`NORMAL` / `OTA`
- 支持 `0x7D` 控制帧：
  - `ENTER_OTA_MODE`
  - `LEAVE_OTA_MODE`
- OTA mode 策略：
  - 禁止 `drop_oldest`
  - 队列满/写失败直接断链
  - OTA mode 超时自动回 NORMAL

### 4.3 STM32 Bootloader 侧
- 增加 OTA 空闲超时退出机制
- 修复超时依赖问题（tick/刷新条件）
- 保持 PREPARE/DATA/VERIFY/COMMIT 协议语义不变

---

## 5. 测试项与结果

### 5.1 Bootloader 进入 OTA 与超时回跳（实测通过）

#### 现象日志（证据）
```text
[WARN][GATEWAY] cmd reboot_to_bootloader
[BL] boot start
[BOOT] ota request magic detected
[BOOT] send boot hello
[BOOT] wait ota command
[BOOT] ota timeout, jump current app
[BL] jump base=0x08020000 ...
[INFO][MAIN] system init done
```

#### 解释
- 说明 RTC magic 触发 Bootloader OTA 等待路径成功
- 在无 OTA 会话指令时，空闲超时触发，安全回跳 APP
- 回跳后 APP 正常初始化，链路恢复

### 5.2 OTA over tcp_binary 基线（mock）（通过）
- 用例：`tools/integration/test_ota_over_tcp_binary.sh`
- 结果：PASS（任务最终 SUCCESS）

#### 解释
- 说明 tcp_binary 会话内 OTA 传输链路可用
- PREPARE/DATA/VERIFY/COMMIT 序列完整闭环

### 5.3 故障注入矩阵（mock）（通过）
- 用例：`tools/integration/test_ota_over_tcp_binary_fault_matrix.sh`
- 场景：
  - success_baseline
  - nack_seq
  - verify_nack
  - disconnect_mid_transfer
- 结果：全部 PASS

#### 解释
- NACK/断链场景可进入 FAILED/CANCELED 或按重试策略恢复
- 未出现误报 SUCCESS

---

## 6. 问题现象与根因说明（过程记录）

### 6.1 现象 A：Bootloader 等待不超时

#### 初始现象
- 出现 `[BOOT] wait ota command` 后长时间不回跳

#### 根因
1. `HAL_Init()` 缺失导致 tick/timeout 机制不可靠  
2. 空闲计时被“非 OTA 合法帧”误刷新，导致一直续命

#### 修复
- 补 `HAL_Init()`
- 超时逻辑改为读帧超时累加 + 仅 OTA 会话帧刷新计数

#### 修复后现象
- 出现 `[BOOT] ota timeout, jump current app`，问题关闭

### 6.2 现象 B：多机同步后行为不一致风险

#### 现象
- 两台设备“看似同版本”，表现不一致

#### 风险解释
- 常见原因是构建产物路径/烧录对象不一致，而非源码文本不一致

#### 措施
- 使用统一产物与构建标识进行校验（建议 build_tag）

---

## 7. 已知限制
- 当前 A->B / B->A 真板完整证据（地址与版本双向）待补充
- ESP32 变更已在联调版本验证，建议再做一次固件烧录后长稳压测
- 目前故障矩阵以 mock 为主，仍需补充板级断电/弱网实测

---

## 8. 后续建议
1. 完成真板 A->B 与 B->A 双向升级证据采集（版本号+slot地址）  
2. 补充 APP_CONFIRM 生效证据（日志与 metadata）  
3. 执行 WiFi 抖动/TCP 半开/中途断链板级回归  
4. 固化 nightly mock fault matrix 到 CI

---

## 9. 结论
当前版本已验证 OTA over ESP32 核心路径可用，Bootloader OTA 等待与超时回跳逻辑正常，故障注入下状态机行为符合预期，未发现协议语义被破坏。整体可进入下一阶段真板双向 OTA 与长期稳定性验收。
