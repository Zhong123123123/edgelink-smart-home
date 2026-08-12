# 协议说明

## 0. 控制链路总览

当前控制链路遵循下面的调用关系：

`Qt -> HTTP Control API -> Gateway Command Service -> Device -> ACK -> JSON Result`

其中：

- Qt 只负责发 HTTP 请求和展示结果。
- 网关复用已有 `pending command`、ACK 匹配和超时机制。
- 设备侧仍然只处理原有串口 / WiFi 协议，不新增 Qt 私有协议。
- HTTP 控制层只是对已有命令能力的薄封装。

---

本文档描述本项目当前使用的两类协议：

1. **STM32 串口二进制协议**（`link_type=serial`，典型 `device_id=1`）
2. **ESP32 WiFi JSON 行协议**（`link_type=wifi`，典型 `device_id=2`）

---

## 1. STM32 串口二进制协议

网关消费固定格式帧：

`AA 55 | LEN | TYPE | PAYLOAD | CRC16`

- 帧头：`0xAA 0x55`
- `LEN`：1 字节，表示 `TYPE + PAYLOAD` 的总字节数
- `TYPE`：1 字节
- `CRC16`：对 `LEN + TYPE + PAYLOAD` 计算 Modbus CRC16，低字节在前（小端）

### 1.1 TYPE 定义

- `0x01`: `SENSOR_DATA`
- `0x02`: `ALARM_EVENT`
- `0x03`: `HEARTBEAT`
- `0x04`: `COMMAND_ACK`
- `0x05`: `DEVICE_STATUS`

未知类型不会导致网关崩溃，会计入 `unknown_type_frames` 指标。

> 说明：`0x10 COMMAND_REQ` 是网关下行到串口设备的帧类型，不作为上行事件解析。

### 1.2 上行负载定义

#### `TYPE=0x01 SENSOR_DATA`

负载长度固定 8 字节：

1. `device_id`（`uint8`）
2. `temperature_x10`（`int16`，LE）
3. `humidity_x10`（`uint16`，LE）
4. `voltage_mv`（`uint16`，LE）
5. `status`（`uint8`）

#### `TYPE=0x02 ALARM_EVENT`

- `device_id`（`uint8`）
- `alarm_code`（`uint8`）
- 可选扩展字段

#### `TYPE=0x03 HEARTBEAT`

- `device_id`（`uint8`）

#### `TYPE=0x04 COMMAND_ACK`

- `device_id`（`uint8`）
- `command_id`（`uint16`，LE）
- `result`（`uint8`）

#### `TYPE=0x05 DEVICE_STATUS`

- `device_id`（`uint8`）
- `online_flag`（`uint8`）
- `version_major`（`uint8`）
- `version_minor`（可选，`uint8`）

### 1.3 串口下行命令（`TYPE=0x10 COMMAND_REQ`）

网关写入串口，设备执行后回 `TYPE=0x04 COMMAND_ACK`。

负载基础格式：

1. `device_id`（`uint8`）
2. `command_id`（`uint16`，LE）
3. `command_type`（`uint8`）
4. `command_args`（可选）

当前 SmartHome 约定：

- `0x01 set_led`：`args=[on(uint8)]`
- `0x02 set_buzzer`：`args=[on(uint8)]`
- `0x03 set_mode`：`args=[mode(uint8),0=manual/1=auto]`
- `0x04 get_status`：`args=[]`
- `0x05 set_threshold`：`args=[temp_x10(uint16,LE),humi_x10(uint16,LE)]`
- `0x06 set_log_level`：`args=[level(uint8),0=error/1=warn/2=info]`

---

## 2. ESP32 WiFi JSON 行协议

WiFi 节点与网关 `wifi_device_server`（默认 `9100`）通过 TCP 长连接通信。

- 每行一个 JSON（`\n` 分隔）
- `timestamp=0` 时由网关补当前 Unix 毫秒时间

### 2.1 ESP32 上行事件示例

#### `sensor_data`

```json
{
  "device_id": 2,
  "device_name": "wifi-node-02",
  "event_type": "sensor_data",
  "timestamp": 0,
  "temperature": 27.2,
  "humidity": 45.8,
  "voltage": 3.3,
  "status": 1,
  "link_type": "wifi",
  "wifi_rssi": -55,
  "mq2_alarm": 0,
  "ld2402_presence": 1,
  "seq": 1001
}
```

#### `heartbeat`

```json
{
  "device_id": 2,
  "device_name": "wifi-node-02",
  "event_type": "heartbeat",
  "timestamp": 0,
  "link_type": "wifi",
  "wifi_rssi": -55,
  "seq": 1002
}
```

#### `command_ack`

```json
{
  "device_id": 2,
  "device_name": "wifi-node-02",
  "event_type": "command_ack",
  "timestamp": 0,
  "command_id": 301,
  "command_result": 0,
  "payload_summary": "cmd_id=301,result=0",
  "link_type": "wifi",
  "wifi_rssi": -55,
  "seq": 1003
}
```

### 2.2 网关到 WiFi 节点下行命令

WiFi 命令使用 JSON，不走串口二进制 `DEVCMD`。

```json
{
  "type": "command",
  "device_id": 2,
  "command_id": 301,
  "command_type": "get_status",
  "params": {},
  "timeout_ms": 5000
}
```

`set_led` 示例：

```json
{
  "type": "command",
  "device_id": 2,
  "command_id": 302,
  "command_type": "set_led",
  "params": { "value": 1 },
  "timeout_ms": 5000
}
```

### 2.3 HTTP 控制 API

网关监控服务额外提供通用控制入口：

- `POST /api/command`
- `GET /api/commands/recent`
- 语义化包装接口：
  - `POST /api/device/{id}/led`
  - `POST /api/device/{id}/mode`
  - `POST /api/device/{id}/threshold`
  - `POST /api/device/{id}/status/query`

`POST /api/command` 的请求体与 Qt 控制页一致，示例：

```json
{
  "device_id": 1,
  "command_type": "set_led",
  "args": { "on": true },
  "timeout_ms": 3000
}
```

返回值包含 `ok`、`command_id`、`status`、`latency_ms` 和 `error`。  
最近命令记录通过 `GET /api/commands/recent` 返回，便于前端展示操作日志。

---

## 3. 网关统一事件与监控字段

无论串口还是 WiFi，最终都会进入统一 `SensorData` 结构并走同一上传链路。

当前常见扩展字段：

- 通用：`frame_type`、`payload_summary`、`command_id`、`command_result`
- WiFi：`link_type=wifi`、`wifi_rssi`、`wifi_connected`、`wifi_last_seen_ms`、`seq`
- ESP32 传感器扩展：`mq2_alarm`、`ld2402_presence`
- STM32 状态拆解：`led_on`、`alarm_on`、`sensor_valid`、`auto_mode`

---

## 4. 容错与指标

解析阶段关键指标：

- `unknown_type_frames`
- `dropped_bytes`
- `wifi_json_parse_ok`
- `wifi_json_parse_fail`
- `wifi_unknown_device`

运行时可通过 `/metrics` 获取 Prometheus 格式指标。
