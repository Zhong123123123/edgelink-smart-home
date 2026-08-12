# 架构设计

本项目是多传输接入网关：串口链路与 WiFi 链路并行接入，统一汇聚到同一事件模型与上传出口。

## 1. 总体链路

- `device_id=1`（STM32）：`serial -> FrameParser -> SensorData`
- `device_id=2`（ESP32）：`wifi tcp json -> parseWifiJsonEvent -> SensorData`
- 统一进入 `DeviceRegistry + RuntimeStats + Uploader(CachedUploader)`

## 2. 线程与职责

1. 串口采集线程（A）
- 打开 Linux 串口设备（`termios`）。
- 连续读取字节流并送入队列。

2. 协议解析线程（B）
- 消费串口字节流。
- 执行帧同步、长度校验、CRC16 校验。
- 产出结构化 `SensorData` 事件。

3. WiFi 设备服务线程（W）
- 监听 `wifi_device_server.listen_host:listen_port`（默认 `0.0.0.0:9100`）。
- 管理 ESP32 TCP 会话（连接、断开、最近活跃时间、RSSI 等）。
- 解析 JSON 行上报并转成 `SensorData`。

4. 上报线程（C）
- 处理统一事件流并进行 `device_id -> device_name/topic_suffix` 映射。
- 调用 `IUploader` 发往 TCP/MQTT。
- 失败时由 `CachedUploader` 落盘并自动补发。

5. 命令下行线程（E）
- 监听 `command.bind_host:port`（默认 `9001`）。
- 按 `device_id` 路由下行：
  - 串口设备：走 `DEVCMD/HEX/TEXT` 串口路径
  - WiFi 设备：走 JSON 命令下发到对应 TCP session
- 复用统一 ACK 跟踪与超时机制。

6. 心跳线程（F）
- 周期上报网关健康状态（uptime、计数器等）。

7. 监控线程（G）
- 提供 `/api/status`、`/api/devices`、`/api/recent`、`/metrics`。
- 页面 `/` 展示仪表盘，支持 recent 按 `device_id` 过滤。

8. 配置热加载控制（主线程）
- 支持 `SIGHUP` 与轮询配置变更。
- 采用“停旧启新，失败回滚”。

## 3. 核心模块

- `serial/SerialPort`：串口 I/O
- `protocol/FrameParser`：串口二进制解析
- `device/WifiTcpDeviceServer`：WiFi 设备 TCP 接入与会话管理
- `runtime/ThreadSafeQueue`：线程间队列
- `uploader/IUploader`、`TcpUploader`、`MqttUploader`、`CachedUploader`
- `device/DeviceRegistry`：设备在线状态和元信息
- `runtime/RuntimeStats`：计数器指标
- `app/AppController`：生命周期编排

## 4. 协议与路由原则

- 不改 STM32 既有串口协议定义。
- WiFi 设备命令使用 JSON，不复用串口 `TYPE=0x10` 二进制协议。
- 两类设备最终统一到同一事件结构和上传链路。

## 5. 当前扩展字段

在 monitor `/api/recent` 中：

- WiFi 侧：`mq2_alarm`、`ld2402_presence`、`wifi_rssi`
- STM32 状态拆解：`led_on`、`alarm_on`、`sensor_valid`、`auto_mode`

## 6. 数据流示例

- 串口：`STM32 -> /dev/ttyUSBx -> serial_gateway -> tcp_receiver`
- WiFi：`ESP32 -> TCP:9100 -> serial_gateway -> tcp_receiver`

## 7. 可靠性机制

- 串口 CRC16 + 流式重同步
- WiFi JSON 解析失败计数，不崩溃
- 设备在线状态超时与最后活跃时间跟踪
- 下行命令 ACK/超时/失败/迟到 ACK 统一计数
- 上报失败缓存与补发
