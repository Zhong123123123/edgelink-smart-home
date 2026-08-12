# ESP32 Wi-Fi Business Node

ESP32 业务节点负责 MQ-2 数字告警与 LD2402 人体存在状态采集，并通过 MQTT（默认）或 TCP JSON 接入 Linux 网关。节点包含 Wi-Fi、网络、传感器、周期上报和 LED 五类 FreeRTOS 任务，支持心跳、远程控制、状态查询及 HTTP OTA 状态回传。

## 配置

复制示例配置并填写本地网络信息：

```bash
cp config.h.example config_local.h
```

`config_local.h` 已被 Git 忽略。`config.h` 只包含安全默认值，并在本地配置存在时自动加载。

常用选项：

- `ACTIVE_COMM_MODE`：选择 MQTT 或 TCP JSON。
- `SENSOR_SIM_MODE`：选择真实 GPIO 或模拟数据。
- `ESP32_OTA_REAL`：选择 OTA 状态模拟或真实 HTTP 下载写入。

Arduino 环境需安装 ESP32 core、PubSubClient，并根据目标板选择对应的 ESP32-S3 配置。网关侧协议与验收说明见 [docs/serial_gateway管理mqtt设备设计与验收.md](docs/serial_gateway管理mqtt设备设计与验收.md)。

