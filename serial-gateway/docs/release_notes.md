# 发布说明

## 版本状态

`serial-gateway` 已进入可用于演示与工程交付的阶段，当前重点能力为“串口 + WiFi 双链路接入”：

- 串口设备接入（STM32，`device_id=1`）
- WiFi 设备接入（ESP32，`device_id=2`）
- 统一事件模型与统一上行链路
- 命令下发与 ACK 跟踪
- 本地缓存补发、运行监控与热加载

## 本次主要更新

1. WiFi 第二节点接入
- 新增 WiFi 设备 TCP 接入服务（默认 `9100`）。
- 支持 ESP32 主动连接并上报 JSON 行协议（`sensor_data` / `heartbeat` / `command_ack`）。
- WiFi 事件统一转换后进入现有网关处理链路，不影响原串口链路。

2. 设备状态与指标扩展
- 设备状态新增 `link_type`、`wifi_connected`、`wifi_rssi`、`wifi_last_seen_ms` 等字段。
- `/api/devices` 可同时看到串口与 WiFi 设备在线状态。
- `/metrics` 增加 WiFi 相关指标：
  - `sg_wifi_clients_connected`
  - `sg_wifi_devices_online`
  - `sg_device_online{device_id=...}`

3. 命令路由升级
- `device_id=1`（串口）继续走原有二进制命令路径（`DEVCMD`）。
- `device_id=2`（WiFi）走 JSON 命令下发路径（`RAW`）。
- 统一复用 ACK/超时/失败/迟到 ACK 统计机制。

4. monitor 页面优化
- 首页升级为更易读的仪表盘布局。
- `Recent Samples` 支持按 `device_id` 过滤（`All / 1 / 2`）。
- 保留 `/api/status`、`/api/devices`、`/api/recent`、`/metrics` 接口兼容性。

5. 测试与文档完善
- 已补充 WiFi mock 与多传输联调脚本说明。
- 更新部署、协议、演示命令、发布清单为中文并同步当前实现。

## 验证范围（建议）

- 单元测试：`ctest --test-dir build --output-on-failure`
- 冒烟测试：
  - `./tests/smoke.sh`
  - `./tests/recovery.sh`
  - `./tests/command_smoke.sh`
  - `./tests/heartbeat_smoke.sh`
  - `./tests/reload_smoke.sh`
  - `./tests/monitor_smoke.sh`
- WiFi 专项：
  - `./tools/integration/test_wifi_node.sh`
  - `./tools/integration/test_multi_transport.sh`

## 已知限制

- WiFi 设备下行命令必须使用 `RAW JSON`，不能用 `DEVCMD`。
- MQTT 冒烟测试依赖本地 `mosquitto` 可执行文件。
- 当前 YAML 解析器为轻量实现，适配现有配置风格。

## 后续建议

- 命令鉴权与审计（ACL/日志）
- TCP/MQTT 可选 TLS
- monitor 页面增加设备趋势图与错误聚合视图
- 为 WiFi 节点补充更多硬件传感器实测用例
