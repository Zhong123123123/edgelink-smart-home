# serial-gateway

`serial-gateway` 是一个基于 Linux + C++17 的串口边缘网关，用于将串口设备数据解析后上报到 TCP/MQTT，并提供缓存补发、命令下发、监控与多实例能力。

## 功能总览

- 串口采集：`termios` 读取串口字节流
- 协议解析：流式组帧 + CRC16 校验（支持多事件类型）
- 上报后端：TCP JSON、MQTT（`libmosquitto` 可用时启用真实发布）
- 可靠性：失败落盘缓存、重连后自动补发
- 命令下发：TCP 命令端口支持 `HEX/TEXT` 与 `TRACK_HEX/TRACK_TEXT`
- 设备状态：在线/离线判定、最近错误、设备级指标
- 观测能力：`/api/status`、`/api/recent`、`/api/devices`、`/metrics`
- 多串口实例：`serials` 一份配置启动多条采集链路
- 配置热加载：`SIGHUP` 或轮询文件改动

## 目录结构

```text
serial-gateway/
├── CMakeLists.txt
├── config/
│   └── gateway.yaml
├── docs/
│   ├── architecture.md
│   ├── deployment.md
│   ├── protocol.md
│   └── demo_commands.md
├── include/
├── src/
├── scripts/
├── systemd/
├── tests/
└── tools/
```

## 快速开始

### 1) 构建

```bash
cmake -S . -B build
cmake --build build -j
```

可选构建参数：

- `-DSERIAL_GATEWAY_ENABLE_MQTT=ON|OFF`：启用/关闭 MQTT 真实后端探测
- `-DSERIAL_GATEWAY_BUILD_TESTS=ON|OFF`：是否编译测试
- `-DSERIAL_GATEWAY_BUILD_TOOLS=ON|OFF`：是否编译联调工具

### 2) 一键本地联调

```bash
./scripts/run_local_demo.sh
```

该脚本会启动：

- 虚拟串口桥接（`pty_bridge`）
- TCP 接收端（`tcp_receiver`）
- 网关主程序（`serial_gateway`）
- 模拟设备（`fake_sensor`）

### 3) 手动分步联调

```bash
./scripts/create_virtual_serial.sh /tmp/ttyV0 /tmp/ttyV1
./build/tcp_receiver 9000
./build/serial_gateway --config=config/gateway.yaml
./build/fake_sensor /tmp/ttyV0 500 115200
```

## 协议概览

串口帧格式：

`AA 55 | LEN | TYPE | PAYLOAD | CRC16`

- `LEN`：`TYPE + PAYLOAD` 长度
- `CRC16`：对 `LEN+TYPE+PAYLOAD` 计算 Modbus CRC16（小端）

当前支持类型：

- `0x01 SENSOR_DATA`
- `0x02 ALARM_EVENT`
- `0x03 HEARTBEAT`
- `0x04 COMMAND_ACK`
- `0x05 DEVICE_STATUS`

详细字段定义见 [docs/protocol.md](docs/protocol.md)。
与 SmartHome 固件的对接步骤见 [docs/smarthome_integration.md](docs/smarthome_integration.md)。

## 配置说明

默认配置文件为 `config/gateway.yaml`，常用段如下。

### serial / serials

- `serial`：单实例串口配置
- `serials`：多实例配置（非空时按多实例模式启动）

`serials` 生效时：

- 每实例独立缓存文件（自动追加实例后缀）
- MQTT client_id 自动追加实例后缀
- 仅第 1 个实例启用 `command` 和 `heartbeat`
- `monitor.port` 自动按实例索引偏移（`base + idx`）

### uploader

- `type`: `tcp` 或 `mqtt`
- `host` / `port`：上报目标
- `disk_cache_enabled`：失败是否落盘
- `disk_cache_file` / `disk_cache_max_lines`：缓存文件与上限
- `replay_batch_size` / `replay_pause_ms`：补发节流
- `connect_timeout_ms` / `reconnect_initial_ms` / `reconnect_max_ms`：重连参数

MQTT 相关字段：

- `mqtt_topic`
- `mqtt_heartbeat_topic`
- `mqtt_client_id`
- `mqtt_username` / `mqtt_password`
- `mqtt_qos`
- `mqtt_keepalive_sec`
- `mqtt_clean_session`
- `mqtt_max_inflight`

### runtime

- `queue_capacity`
- `stats_interval_sec`
- `drop_unknown_devices`
- `device_offline_timeout_sec`
- `cache_backlog_warn_threshold`

### command

- `enabled`
- `bind_host`
- `port`
- `client_timeout_ms`

### heartbeat

- `enabled`
- `interval_sec`
- `gateway_id`

### reload

- `enabled`
- `check_interval_sec`

### monitor

- `enabled`
- `bind_host`
- `port`
- `recent_capacity`

### devices

设备映射字段：

- `id`
- `name`
- `enabled`
- `topic_suffix`

## 命令下发接口

命令端口协议为一行一条：

- `HEX <hexbytes>`
- `TEXT <text>`
- `TRACK_HEX <device_id> <command_id> <command_type> <timeout_ms> <hexbytes>`
- `TRACK_TEXT <device_id> <command_id> <command_type> <timeout_ms> <text>`

返回示例：

- 成功写入：`OK wrote=...`
- ACK 成功：`OK ack device_id=... command_id=... result=... wrote=...`
- 失败：`ERR ...`

`tools/command_sender` 支持 `HEX` / `TEXT`，以及 `DEVCMD`（自动组设备命令帧并使用 `TRACK_HEX` 等待 ACK）。  
也可直接使用 `nc` 发送 `TRACK_*`：

```bash
echo "TRACK_HEX 1 100 reboot 2000 AA55040101640001BEEF" | nc 127.0.0.1 9001
```

设备命令示例（SmartHome 串口设备）：

```bash
./build/command_sender 127.0.0.1 9001 DEVCMD 1 100 2000 set_led 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 101 2000 set_mode 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 102 2000 set_threshold 35.0 80.0
./build/command_sender 127.0.0.1 9001 DEVCMD 1 103 2000 get_status
```

## 监控与指标

默认监控地址：

```text
http://127.0.0.1:9010/
```

接口：

- `GET /api/status`：实例状态、吞吐、缓存、重连、资源占用
- `GET /api/recent`：最近样本
- `GET /api/devices`：设备在线状态/错误摘要
- `GET /metrics`：Prometheus 格式指标

主要指标（按实例）：

- `sg_frames_rx_total`
- `sg_frames_ok_total`
- `sg_frames_crc_fail_total`
- `sg_frames_unknown_total`
- `sg_bytes_dropped_total`
- `sg_upload_ok_total`
- `sg_upload_fail_total`
- `sg_cache_enqueue_total`
- `sg_cache_replay_ok_total`
- `sg_cache_replay_fail_total`
- `sg_cache_backlog`
- `sg_reconnect_total`
- `sg_last_reconnect_ms`
- `sg_devices_online`
- `sg_devices_offline`
- `sg_device_online`
- `sg_device_report_total`

## 测试与验证

### 单元测试

```bash
ctest --test-dir build --output-on-failure
```

### 冒烟与专项脚本

```bash
./tests/smoke.sh
./tests/recovery.sh
./tests/mqtt_smoke.sh
./tests/command_smoke.sh
./tests/heartbeat_smoke.sh
./tests/reload_smoke.sh
./tests/monitor_smoke.sh
./tests/multi_instance_smoke.sh
```

### 故障注入与压测

## EdgeOTA（新增）

### 项目定位
在现有 `serial-gateway` 工程中扩展 OTA 生命周期治理能力，统一管理 STM32 串口节点和 ESP32 WiFi 节点的固件版本与升级任务。

### 已实现功能
- 固件仓库：`FirmwareStore`（manifest 校验、CRC32 校验、固件入库/查询/删除）。
- 任务状态机：`CREATED` 到 `SUCCESS/FAILED/CANCELED/ROLLBACK`。
- 任务持久化：`PersistentStore`（JsonFileStore 形态，接口可替换 SQLite）。
- STM32 OTA 适配器：`AA55` 帧、`PREPARE/DATA/VERIFY/COMMIT`、ACK/NACK、重试。
- ESP32 OTA 适配器：`ota_start` JSON 下发与 `ota_status` JSON 解析。
- HTTP API：`/api/ota/tasks`、`/api/ota/tasks/{id}`、`/events`、`/cancel`、`/retry`。
- 自动执行：`POST /api/ota/tasks` 创建后自动异步执行 OTA（按 `device_type` 选择适配器）。
- OTA 可观测性：`/metrics` 提供 `ota_*` 指标。
- 故障注入：`fake_bootloader` 支持 NACK、断连、verify fail。

### 快速体验
```bash
./scripts/run_ota_local_demo.sh
./build/otactl ota list
```

### HTTP API 快速示例
```bash
curl -X POST http://127.0.0.1:9010/api/ota/tasks \
  -H 'Content-Type: application/json' \
  -d '{"device_id":1,"device_type":"stm32f407-smarthome","firmware_id":"stm32f407-smarthome-1.0.1","target":"127.0.0.1:19090"}'
curl http://127.0.0.1:9010/api/ota/tasks
curl http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/events
curl -X POST http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/cancel
curl -X POST http://127.0.0.1:9010/api/ota/tasks/<task_uuid>/retry
```

### 实施边界
- 当前网关 OTA 链路以仿真验证为主，Bootloader/ESP32 真 OTA 仍需硬件闭环验证。
- 交付对照与硬件待验证项见 `docs/ota_delivery_checklist.md`。

### 简历亮点（可讲）
- STM32 OTA 分包协议设计（seq/offset/ACK/NACK/重试）。
- 异构设备统一 OTA 状态机建模。
- 任务持久化与重启恢复基础框架。
- 故障注入与 metrics 可观测性闭环。

```bash
./scripts/run_fault_injection.sh
./scripts/multi_instance_fault_injection.sh
./scripts/multi_instance_fault_injection.sh 9930 4 5 mqtt 18893 4
./scripts/run_stress_test.sh 20 5
```

### 多实例状态采集

```bash
./scripts/collect_multi_instance_status.sh 127.0.0.1 9010 3
./scripts/collect_multi_instance_status.sh --watch --interval 2 --count 5 127.0.0.1 9010 3
./scripts/collect_multi_instance_status.sh --watch --interval 1 --csv-file /tmp/sg_status.csv 127.0.0.1 9010 3
```

CSV 当前输出列：

`timestamp,instance_index,port,serial_instance,devices_online,received_frames,upload_success,upload_failed,cache_backlog,status_file,devices_file,metrics_file`

## MQTT 启用说明

安装开发库后重新构建：

```bash
sudo apt-get install -y libmosquitto-dev
cmake -S . -B build -DSERIAL_GATEWAY_ENABLE_MQTT=ON
cmake --build build -j
```

未检测到 `libmosquitto` 时，MQTT 上传器将退化为占位实现（构建阶段会给出提示）。

## systemd 部署

安装：

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j
sudo cmake --install build
```

启停服务：

```bash
sudo ./scripts/install_systemd_service.sh
sudo systemctl start serial-gateway.service
sudo systemctl status serial-gateway.service
journalctl -u serial-gateway.service -f
```

资源限制：

```bash
sudo ./scripts/set_systemd_limits.sh --memory-max 300M --cpu-quota 50% --tasks-max 256
sudo ./scripts/set_systemd_limits.sh --reset
```

卸载：

```bash
sudo ./scripts/uninstall_systemd_service.sh
```

## 常见问题

- `serial open failed`：检查设备路径和串口权限（`dialout` 组、`udev` 规则）
- `upload failed`：检查后端服务可达性、端口、防火墙
- `cache backlog high`：说明持续失败或补发速度不足，优先排查网络与目标吞吐
- `device offline`：设备长时间无有效上报或线路故障
- monitor 端口冲突：多实例模式下确认端口偏移范围未被占用

## 相关文档

- 协议说明：`docs/protocol.md`
- 架构说明：`docs/architecture.md`
- 部署运维：`docs/deployment.md`
- 演示命令：`docs/demo_commands.md`
- 发布记录：`docs/release_notes.md`
- 变更历史：`CHANGELOG.md`

## 打包发布

```bash
./scripts/release_bundle.sh
```

输出目录：`dist/`

- `serial-gateway-<ver>-src.tar.gz`
- `serial-gateway-<ver>-linux-x86_64.tar.gz`
- `serial-gateway-<ver>-linux-armhf.tar.gz`
- `SHA256SUMS`


## STM32 over ESP32 WiFi 通信适配器

- ESP32 是轻量帧感知通信适配器，不是业务处理器。
- 业务 ACK 仍由 Linux Gateway 和 STM32 端到端完成。
- serial 链路保留为调试/回退。
- `tcp_binary` 优先级高于 `serial`。

## STM32 OTA over ESP32 WiFi Adapter

- 升级链路：`Linux OTA Manager -> TCP Binary -> ESP32 Adapter -> UART -> STM32 Bootloader`
- OTA transport 支持 `serial` 与 `tcp_binary`，可通过 API/CLI 选择。
- OTA 期间设备进入会话独占，普通命令返回 `device_in_ota`。
- ESP32 增加 `NORMAL/OTA` bridge mode：
  - NORMAL：允许 `drop_oldest`
  - OTA：禁止丢帧，队列满或写失败直接断链
- Linux 与 ESP32 通过 `TYPE=0x7D` control frame 进入/退出 OTA mode。
- Bootloader 在“RTC magic 进入 OTA”路径支持空闲总超时，超时后回当前可启动 APP。

详细设计与流程见 [docs/ota_over_esp32.md](docs/ota_over_esp32.md)。  
测试结果见 [docs/ota_over_esp32_test_report.md](docs/ota_over_esp32_test_report.md)。
