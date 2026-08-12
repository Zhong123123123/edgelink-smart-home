# SmartHome F407 两轮改造复盘（Round1 + Round2）

## 1. 修改文件清单

### 1.1 第一轮（SPI Flash / StorageTask / EventLog）
- `6_ModuleDrives/driver_w25qxx.c`, `6_ModuleDrives/driver_w25qxx.h`
  - 新增 W25Qxx SPI 驱动（JEDEC ID、读写、擦除、状态轮询）。
- `5_Platform/platform_storage.c`, `5_Platform/platform_storage.h`
  - 平台层封装 Flash 访问接口。
- `2_Device/dev_storage.c`, `2_Device/dev_storage.h`
  - 设备抽象层封装存储读写/擦除。
- `1_App/app_storage.c`, `1_App/app_storage.h`
  - 新增 StorageTask、Event/Fault 记录写入、启动扫描。
- `8_Core/board_pins.h`
  - 增加 SPI Flash 引脚定义。
- `8_Core/stm32f4xx_hal_conf.h`
  - 打开 SPI HAL 宏并包含 SPI 头。
- `1_App/main.c`
  - 启动 Storage 任务。

### 1.2 第二轮（OfflineCache / IWDG / HardFault / RTC Backup）
- `1_App/app_gateway.c`
  - 接入 OfflineCache 回放与 ACK 出队流程。
  - 增加 replay 节流（避免刷屏）与 pending 管理。
  - 修复 replay 重复发送同一 cache 记录导致 pending 塞满的问题。
- `1_App/app_storage.c`
  - 新增缓存写入/读取/ACK 标记接口（`Storage_WriteCache/Storage_PeekCache/Storage_MarkCacheAck`）。
  - 修复缓存区写入策略：仅在非擦空时擦除，避免误擦同扇区历史记录。
  - 增加 cache 头异常诊断日志与重扫自愈。
- `1_App/app_storage.h`
  - 暴露 cache 相关接口与 `Storage_GetCacheBacklog`。
- `1_App/app_watchdog.c`, `1_App/app_watchdog.h`
  - 实现任务心跳表、软件看门狗巡检、IWDG 喂狗控制。
- `1_App/main.c`
  - 启动时接入 `FaultDiag_InitAtBoot()`。
  - 注册关键任务到 watchdog 并启动 watchdog 任务。
- `1_App/app_sensor.c`, `app_alarm.c`, `app_display.c`, `app_key.c`, `app_led.c`, `app_gateway.c`, `app_storage.c`
  - 接入 `Watchdog_Kick()`。
- `8_Core/fault_diag.c`, `8_Core/fault_diag.h`
  - 实现 HardFault 快照采集、CRC 校验、重启后落盘与日志输出。
- `8_Core/stm32f4xx_it.c`
  - HardFault 裸函数跳转到 `FaultDiag_HardFaultCapture`。
- `5_Platform/platform_backup.c`, `5_Platform/platform_backup.h`
  - 实现 RTC Backup 读写、复位原因识别与字符串化。
- `config.h`
  - 增加调试开关：`ENABLE_IWDG`、`ENABLE_WDG_INJECTION`、`ENABLE_HARDFAULT_INJECTION`。
- `Project/smarthome_f407_app_a.sct`, `Project/smarthome_f407_app_b.sct`
  - 修正 `UNINIT` 语法，支持 `.noinit` 区域链接。

## 2. 功能实现说明

### 2.1 第一轮
- SPI Flash 驱动打通：可读 JEDEC、可扇区擦除、可页写。
- StorageTask 启动扫描 Event 区：统计 `valid` 与 `last_seq`。
- 启动写入 boot event。
- EventLog 支持连续写入，重启后可恢复扫描状态。

### 2.2 第二轮

#### OfflineCache
- 发送失败或 ACK 超时：写入 cache 分区。
- 网关恢复后：按 cache 记录顺序 replay。
- 收到 report ACK 后：按 `cache_seq` 出队并递减 backlog。
- backlog 清零后输出 `replay done backlog=0`。

#### IWDG + 任务心跳
- 每个关键任务定期 `Watchdog_Kick()`。
- WatchdogTask 周期巡检 `last_kick`。
- 全部任务健康时喂 IWDG；异常时停止喂狗触发复位。
- 记录 `last_dead_task` 到 RTC Backup。

#### HardFault + RTC Backup
- HardFault 中仅做寄存器快照采集、备份标志写入、系统复位。
- 重启后校验快照并输出关键信息（PC/LR/CFSR/HFSR）。
- 通过 `Storage_WriteFault()` 将故障文本持久化。

## 3. 编译结果

- Keil F407 App A/B、Bootloader 均可编译通过。
- 关键修复：`.sct` 文件 `UNINIT` 语法错误已修复。

## 4. 上板验证步骤（已执行）

### 4.1 第一轮验证
1. 启动后确认 `flash init ok`、`scan event valid=... last_seq=...`。
2. 复位后确认 `last_seq` 递增、再次写入 event（如 `seq=2,3...`）。
3. 验证 EventLog 不因单条异常记录导致系统卡死。

### 4.2 第二轮验证
1. 断网/关网关，观察 `report ack timeout` 与 `cache write`，确认 backlog 递增。
2. 打开网关，观察 replay：
   - `replay send cache_seq=...`
   - `report ack seq=...`
   - `cache ack seq=... backlog=...`
   - `replay done backlog=0`
3. 验证回放完成后实时链路：`report send` 与 `report ack` 持续一一对应。

## 5. 验收日志样例（本次实测）

### 5.1 第一轮样例
```text
[INFO][STORAGE] flash init ok jedec=0x00684018 size=16384KB
[INFO][STORAGE] scan event valid=2 last_seq=2
[INFO][STORAGE] event write seq=3 len=10
```

### 5.2 第二轮样例
```text
[INFO][STORAGE] cache ack seq=100 backlog=0
[INFO][GW] replay ack cache_seq=100 backlog=0
[INFO][GW] replay done backlog=0
[INFO][GW] report send seq=46
[INFO][GW] report ack seq=46
[INFO][GATEWAY] diag ... pending=0
```

## 6. 风险与注意事项

- `ENABLE_IWDG` 默认 `0`，防止调试阶段频繁复位。
- HardFault 注入默认关闭（`ENABLE_HARDFAULT_INJECTION=0`）。
- 若网关未回 `type=0x06` ACK，cache 不会出队，backlog 会持续增长。
- ACK 必须回原始 `report_seq`（小端），否则 replay 无法完成。
- SPI 走 `PB3/PB4/PB5 + PB0`（CS）时，需确认与板级调试/外设复用无冲突。

## 7. 当前验收结论

- Round1：通过（SPI Flash + StorageTask + EventLog 已上板闭环）。
- Round2 OfflineCache：通过（已完成 backlog 从 N 到 0 回放）。
- Round2 实时链路：通过（send/ack 连续匹配，pending=0）。
- Round2 IWDG/HardFault：代码已落地，建议按计划再做一次专门注入验收并归档日志。

## 8. 简历表述建议（可直接使用）

- 基于 STM32F407 外部 SPI Flash 设计本地可靠缓存与事件日志模块，采用分区管理、序号与 CRC 校验，实现断链落盘、恢复后按序补发与异常追踪。
- 设计设备侧故障自恢复机制：基于 FreeRTOS 任务心跳控制 IWDG 喂狗，实现任务卡死自动复位；实现 HardFault 快照与 RTC Backup 状态保留，重启后持久化故障信息。

## 9. 详细排查过程（按问题时间线）

### 9.1 μVision 工程打不开（`Cannot read project file ...uvprojx`）
- 现象：Keil 报错无法读取 `SmartHome_F407.uvprojx`。
- 排查：对比备份工程目录与当前目录，确认当前工程文件损坏/不完整。
- 处理：切换到可打开的备份工程 `D:\CDevelop\SmartHome0520backup` 作为基线，重新同步后续改动。
- 结果：工程恢复可打开。

### 9.2 第一轮初期编译失败（SPI 相关符号未定义）
- 现象：`SPI_HandleTypeDef`、`HAL_SPI_Transmit` 等未声明。
- 排查：确认 `stm32f4xx_hal_conf.h` 未开启 SPI HAL；工程未包含 `stm32f4xx_hal_spi.c`。
- 处理：
  - 在 `8_Core/stm32f4xx_hal_conf.h` 打开 `HAL_SPI_MODULE_ENABLED` 并引入 SPI 头。
  - 在 Keil 工程中加入 `stm32f4xx_hal_spi.c` 与新增 `storage` 相关源文件。
- 结果：第一轮编译通过，SPI 驱动链路建立。

### 9.3 A 槽启动早期 `malloc failed free_heap=800`
- 现象：任务创建后早期 OOM，系统停在 `vApplicationMallocFailedHook`。
- 排查：对比 A/B 任务集合与新增模块后堆占用，确认 FreeRTOS 堆配置不足。
- 处理：提升 `configTOTAL_HEAP_SIZE`（从 10KB 逐步调高到可运行值）。
- 结果：A 槽稳定启动，任务可全部拉起。

### 9.4 Bootloader 强制槽位宏判断异常
- 现象：调试强制跳槽逻辑行为不符合预期。
- 排查：`#if (BL_DEBUG_FORCE_BOOT_SLOT == BL_SLOT_A/B)` 在预处理阶段与枚举常量组合有兼容性问题。
- 处理：改为字面值比较 `0U/1U`。
- 结果：强制跳 A/B 调试可控，后续回退为常规启动策略。

### 9.5 `.sct` 链接脚本报错（`Expected '{', found 'U...'`）
- 现象：链接阶段失败，定位到 `smarthome_f407_app_a.sct` 第 10 行。
- 排查：`UNINIT` 关键字位置不符合当前 ARM Linker 语法。
- 处理：将 `RW_NOINIT ... 0xSIZE UNINIT` 改为 `RW_NOINIT ... UNINIT 0xSIZE`（A/B 两份脚本同时修复）。
- 结果：链接恢复正常，`.noinit` 区可用。

### 9.6 ESP32 桥接无有效帧（`valid_frames=0`）
- 现象：ESP32 WiFi/TCP 正常，但长期 `valid_frames=0, crc_errors=0`。
- 排查：读取 `esp32_comm_adapter` 工程配置，确认桥接使用 `Serial2 RX=16 TX=17`；对照 STM32 侧 `USART3 PB10/PB11`。
- 根因：串口线序接反。
- 处理：按 `PB10(TX)->GPIO16(RX)`, `PB11(RX)<-GPIO17(TX)`, `GND共地` 重接。
- 结果：ESP32 出现 `frame ok type=1/3/5`，链路打通。

### 9.7 在线后仍持续 `report ack timeout`（backlog 只增不减）
- 现象：设备持续写 cache，`replay start` 频繁。
- 排查：
  - 增加 replay/peek 诊断日志。
  - 确认网关未返回 `type=0x06` ACK 或 ACK 条件不满足时，设备无法出队。
- 处理：
  - 增加 replay 节流与状态日志，避免无效刷屏。
  - 明确网关 ACK 要求：必须回原 `report_seq`（小端），历史 seq 也要 ACK。
- 结果：网关侧修正后，开始出现 `report ack`。

### 9.8 `cache peek invalid hdr ... 0xFFFFFFFF backlog>0`
- 现象：`backlog` 显示有积压，但 `cache_read_addr` 读到全 `0xFF`，回放卡死。
- 排查：检查 `storage_write_cache_record`，发现每次写 cache 都先扇区擦除，可能擦掉同扇区旧记录。
- 根因：缓存写策略错误（无脑擦除导致历史记录丢失，计数与实际数据不一致）。
- 处理：
  - 改为“仅当写入区域非擦空时才擦除”。
  - `Storage_PeekCache` 异常头时触发 cache 重扫自愈。
- 结果：`cache peek` 恢复有效记录读取。

### 9.9 replay 重复发送同一条，`pending` 被占满
- 现象：同一 `cache_seq=29` 在单次 replay 窗口内重复发送，`pending=8` 很快打满。
- 排查：replay 发送前未检查“该 cache 记录是否已在 in-flight pending 中”。
- 处理：新增 `gw_pending_find_replay_cache_seq()`，同一 cache 记录在 ACK/超时前不重复入队。
- 结果：不再被同一记录塞满 pending，回放按 ACK 节奏推进。

### 9.10 最终闭环验证
- 关键日志：
  - `cache ack seq=... backlog=...` 持续递减
  - `replay done backlog=0`
  - 后续实时 `report send/ack` 一一对应，`pending=0`
- 结论：OfflineCache 断链积压与恢复补发流程完整打通。
