# 部署与运维指南

本文档用于将 `serial-gateway` 部署到 Linux 主机，并覆盖当前项目常用的“双链路”场景：

- `device_id=1`：STM32 串口设备（`link_type=serial`）
- `device_id=2`：ESP32 WiFi 设备（`link_type=wifi`）

## 1. 环境准备

建议环境：

- Linux（systemd）
- C++17 编译器（`g++`/`clang++`）
- CMake >= 3.16

可选依赖：

- `libmosquitto-dev`（需要真实 MQTT 后端时）

## 2. 构建与安装

```bash
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build -j
sudo cmake --install build
```

安装后关键路径：

- 二进制：`/usr/local/bin/serial_gateway`
- 示例配置：`/usr/local/etc/serial-gateway/gateway.yaml.example`
- systemd 单元：`/usr/local/share/serial-gateway/systemd/serial-gateway.service`

## 3. 配置要点

### 3.1 典型端口

- `9000`：上报到 `tcp_receiver`
- `9001`：命令服务
- `9010`：monitor HTTP
- `9100`：WiFi 设备接入（ESP32 主动连入）

### 3.2 关键配置片段

```yaml
serial:
  instance_name: "primary"
  device: "/dev/ttyUSB0"
  baudrate: 115200

wifi_device_server:
  enabled: true
  listen_host: "0.0.0.0"
  listen_port: 9100

devices:
  - device_id: 1
    name: "sensor-01"
    link_type: "serial"
    topic_suffix: "line-a/sensor-01"
  - device_id: 2
    name: "wifi-node-02"
    link_type: "wifi"
    topic_suffix: "line-a/wifi-node-02"
```

说明：

- `wifi_device_server.enabled=false` 时不启动 WiFi 监听。
- 保留串口链路不变，WiFi 为新增并行链路。

## 4. 启动方式

### 4.1 手工启动（调试推荐）

```bash
mkdir -p logs
nohup ./build/tcp_receiver 9000 > logs/tcp_receiver.log 2>&1 &
nohup sudo ./build/serial_gateway --config=config/gateway.yaml > logs/serial_gateway.log 2>&1 &
```

### 4.2 systemd 启动（生产推荐）

```bash
sudo ./scripts/install_systemd_service.sh
sudo systemctl daemon-reload
sudo systemctl enable serial-gateway.service
sudo systemctl start serial-gateway.service
sudo systemctl status serial-gateway.service
```

日志查看：

```bash
journalctl -u serial-gateway.service -f
```

## 5. 发布前验证

建议顺序：

1. 单元测试
```bash
ctest --test-dir build --output-on-failure
```

2. 冒烟回归
```bash
./tests/smoke.sh
./tests/recovery.sh
./tests/command_smoke.sh
./tests/heartbeat_smoke.sh
./tests/reload_smoke.sh
./tests/monitor_smoke.sh
./tests/multi_instance_smoke.sh
```

3. WiFi 专项
```bash
./tools/integration/test_wifi_node.sh
./tools/integration/test_multi_transport.sh
```

## 6. 运维观察点

### 6.1 HTTP 接口

- `GET /api/status`：实例总体状态
- `GET /api/devices`：设备状态（在线、RSSI、最近上报）
- `GET /api/recent`：最近样本（前端支持按 `device_id` 过滤）
- `GET /metrics`：Prometheus 指标

### 6.2 关键指标

- `sg_wifi_clients_connected`
- `sg_serial_devices_online`
- `sg_wifi_devices_online`
- `sg_device_online{device_id=...}`
- `sg_command_ack_count` / `sg_command_timeout_count` / `sg_command_fail_count`

### 6.3 常见故障定位

1. `serial open failed`：串口路径或权限问题
2. WiFi 设备不在线：检查 `9100` 监听、防火墙、ESP32 `GATEWAY_HOST`
3. `ERR wifi device requires JSON command`：对 WiFi 设备误用了 `DEVCMD`
4. `cache backlog high`：上游不可达或恢复慢

## 7. 回滚建议

- 配置回滚：发送 `SIGHUP`，若新配置加载失败会自动回滚。
- 版本回滚：保留上一版二进制与配置，通过 systemd 重启切换。
