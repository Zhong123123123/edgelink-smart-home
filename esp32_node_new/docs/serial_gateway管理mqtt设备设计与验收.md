# serial-gateway 管理 MQTT 设备设计与验收

## 1. 背景

当前 `esp32_node_new` 已完成从 `TCP JSON` 到 `MQTT` 的节点侧改造。

此时设备连接路径已经从：

```text
ESP32 -> wifi_device_server:9100 -> serial_gateway
```

演进为：

```text
ESP32 <-> MQTT Broker <-> serial_gateway
```

因此，`serial_gateway` 如果要继续“管理”该设备，不能再沿用 `wifi_device_server` 的方式，而应把该设备视为一个 **MQTT 原生设备**。

---

## 2. 当前结论

## 2.1 当前节点模式下，`mqtt_command_bridge` 不是最终管理方案

虽然仓库中已经新增了：

`serial-gateway/tools/mqtt_command_bridge.cpp`

但该工具更适合作为过渡或兼容工具，不适合作为 MQTT 原生设备的最终管理架构。

原因是：

1. 当前 `esp32_node_new` 节点已直接连接 Broker
2. 节点不再挂在 `serial_gateway` 的 `wifi_device_server:9100`
3. `serial_gateway` 无法再把它识别成一个活跃的 `wifi` transport 设备
4. 因而网关 command socket 无法把 MQTT 命令继续转成 WiFi TCP JSON 下发

这说明：

> 对 MQTT 直连节点，网关应通过 MQTT topic 管理，而不是通过 `wifi_device_server` 管理。

---

## 3. 目标

后续要让 `serial-gateway` 真正管理 MQTT 原生设备，目标应定义为：

1. 网关可订阅设备 MQTT 上报
2. 网关可识别设备在线/离线状态
3. 网关可通过 MQTT 下发普通命令
4. 网关可通过 MQTT 下发 OTA 启动命令
5. 网关可接收设备 `command/ack`
6. 网关可接收设备 `ota/status`
7. 以上能力尽量复用现有内部：
   - `DeviceRegistry`
   - `pending command`
   - `HistoryStore`
   - `monitor api`
   - `OtaTaskManager`

---

## 4. 建议的最终架构

推荐架构如下：

```text
ESP32 MQTT Device
    <-> MQTT Broker
    <-> serial-gateway MQTT Device Ingress
    <-> DeviceRegistry / data_queue / command tracking / ota task
```

即：

1. `serial-gateway` 新增 MQTT 设备接入层
2. 将 MQTT 视为一种新的 transport
3. 不再依赖 `wifi_device_server` 管理此类设备

---

## 5. 设备 topic 规划

## 5.1 上行 topic（设备 -> Broker -> 网关）

1. `gateway/{gateway_id}/device/{device_id}/telemetry`
2. `gateway/{gateway_id}/device/{device_id}/status`
3. `gateway/{gateway_id}/device/{device_id}/event`
4. `gateway/{gateway_id}/device/{device_id}/command/ack`
5. `gateway/{gateway_id}/device/{device_id}/ota/status`

## 5.2 下行 topic（网关 -> Broker -> 设备）

1. `gateway/{gateway_id}/device/{device_id}/command/down`
2. `gateway/{gateway_id}/device/{device_id}/ota/start`

---

## 6. 网关侧设计建议

## 6.1 新增 MQTT Device Ingress 模块

建议为 `serial-gateway` 新增一条并行输入链路，例如：

1. `serialLoop`：串口设备
2. `wifiDeviceServerLoop`：TCP JSON WiFi 设备
3. `tcpBinaryLoop`：TCP Binary 设备
4. `mqttDeviceLoop`：MQTT 原生设备

这样 MQTT 设备就成为一种新的 transport。

建议在内部 transport 体系中加入：

1. `serial`
2. `wifi`
3. `tcp_binary`
4. `mqtt`

并纳入现有逻辑：

1. `activeTransportFor(device_id)`
2. `bindActiveTransport(device_id, "mqtt", priority)`
3. `shouldAcceptTransportData(...)`

---

## 6.2 MQTT 上报进入现有数据通道

MQTT 上行消息建议不要绕开现有系统，而是转换为统一内部数据结构后入队。

推荐流程：

1. 订阅 `telemetry/status/event/ack/ota/status`
2. 解析 MQTT topic 和 JSON payload
3. 映射成内部 `SensorData` 或等价事件结构
4. 推入现有 `data_queue_`
5. 复用后续流程：
   - `DeviceRegistry`
   - `HistoryStore`
   - `monitor api`
   - `uploader`

这样 MQTT 设备在网关内部就能与现有设备保持一致的管理方式。

---

## 6.3 MQTT 命令下发进入现有控制链路

现有网关已有：

1. `/api/command`
2. `/api/device/{id}/...`
3. `pending command` 跟踪
4. ACK 匹配机制

MQTT 设备建议接入方式：

1. 外部 HTTP/API 仍生成 `ControlCommandRequest`
2. `runControlCommand()` 判断当前设备活动 transport
3. 若 `active_transport == "mqtt"`
4. 则发布到 MQTT：
   - 普通命令 -> `command/down`
   - OTA 命令 -> `ota/start`
5. 然后继续使用现有 pending command 机制等待 ACK

这样可以避免为 MQTT 设备重写一套独立控制 API。

---

## 6.4 ACK 与 OTA 状态回流

### 命令 ACK

`command/ack` 到来后建议：

1. 提取 `device_id`
2. 提取 `command_id`
3. 提取 `result/code`
4. 调用现有：
   - `completeCommandAck(device_id, command_id, ack_result)`

### OTA 状态

`ota/status` 到来后建议：

1. 提取 `device_id`
2. 提取 `command_id`
3. 提取 `ota_state`
4. 将状态映射到现有 OTA 任务状态机
5. 更新 `OtaTaskManager` / OTA event history

---

## 7. 配置建议

建议不要把 MQTT 设备接入配置塞进当前 `uploader.mqtt_*` 配置块，而是新增独立配置块，例如：

```yaml
mqtt_device_ingress:
  enabled: true
  host: "127.0.0.1"
  port: 1884
  client_id: "serial-gateway-device-ingress"
  username: ""
  password: ""
  gateway_id: "gw001"
  topic_prefix: "gateway"
  keepalive_sec: 60
```

职责区分：

1. `uploader.mqtt_*`
   - 网关向外上传

2. `mqtt_device_ingress.*`
   - 网关接入 MQTT 原生设备

这两个职责不应混在同一配置块中。

---

## 8. 推荐实施顺序

建议按以下顺序开发与验收：

## 第一步：只做 MQTT 上报接入

目标：

1. 网关能订阅设备 `telemetry/status/event`
2. 设备能在 monitor / registry 中显示在线
3. 设备数据能写入 history / recent

此阶段先不做命令下发。

## 第二步：补 MQTT 命令下发

目标：

1. 网关能向 `command/down` 发布命令
2. 能接收 `command/ack`
3. 能复用现有 pending command 机制

## 第三步：补 MQTT OTA

目标：

1. 网关能向 `ota/start` 发布 OTA 启动命令
2. 能接收 `ota/status`
3. 能把 OTA 状态更新回现有 OTA task 体系

---

## 9. 验收标准

## 9.1 第一阶段验收：设备接入

满足以下条件视为通过：

1. 网关订阅后能收到 MQTT 设备的 `telemetry`
2. 网关订阅后能收到 MQTT 设备的 `status`
3. monitor 页面或 API 能看到该设备在线
4. 最近数据和历史记录中能看到该设备数据

## 9.2 第二阶段验收：命令控制

满足以下条件视为通过：

1. 网关经 HTTP/API 发起命令
2. 网关向 `command/down` 发布成功
3. 设备执行命令成功
4. 网关收到 `command/ack`
5. `pending command` 匹配成功

## 9.3 第三阶段验收：OTA 管理

满足以下条件视为通过：

1. 网关经 OTA API 发起任务
2. 网关向 `ota/start` 发布成功
3. 设备收到 OTA 启动命令
4. 网关收到 `ota/status`
5. OTA 状态能体现在任务状态机中
6. 模拟 OTA 和真实 OTA 至少各验证一轮

---

## 10. 当前进度

截至当前版本，第二阶段核心目标已经完成：

1. `esp32_node_new` 已完成 MQTT 直连 Broker 改造
2. `serial-gateway` 已新增 MQTT 原生设备接入层
3. `serial-gateway` 可将设备 `2` 识别为 `mqtt` transport
4. 网关 `/api/command` 已可向 MQTT 设备下发 `set_led`、`get_status`
5. 网关 `/api/ota/tasks` 已可向 MQTT 设备下发 `ota/start`
6. 网关已可接收 MQTT 设备的 `command/ack` 与 `ota/status`
7. `mqtt_command_bridge` 保留为过渡工具，但不再是最终管理路径

---

## 11. 本轮实测结果

### 11.1 设备接入

已验证通过：

1. ESP32 设备能稳定连接 Broker
2. `serial-gateway` 启动后可订阅并识别 MQTT 设备在线
3. 设备 `2` 已被网关绑定为 `active transport = mqtt`

### 11.2 命令控制

已验证通过：

1. `set_led` 可通过网关 HTTP API 下发
2. `get_status` 可通过网关 HTTP API 下发
3. 设备能回 `command/ack`
4. 网关侧 `pending command` 匹配成功

### 11.3 OTA 模拟

已验证通过：

1. 网关可创建 MQTT OTA 任务
2. 网关可向 `gateway/gw001/device/2/ota/start` 发布启动消息
3. 设备可回 `ota/status`
4. 模拟 OTA 后设备重启，并重新接入 Broker
5. 网关任务状态已记录为成功

本轮已确认的一条成功任务：

```text
task_uuid   : ota-1782723717552
device_id   : 2
device_type : esp32-wifi-node
firmware_id : esp32-wifi-node-1.1.0
state       : SUCCESS
```

其任务事件链为：

```text
CREATED
WAIT_DEVICE_ONLINE
PRECHECK
PREPARE_DEVICE
TRANSFERRING
VERIFYING
COMMITTING
REBOOTING
VERSION_CHECK
HEALTH_CONFIRM
SUCCESS
```

### 11.4 恢复验收

已验证通过：

1. 重启 `serial-gateway` 后，MQTT ingress 可自动恢复订阅，设备重新上线
2. 重启 Broker 后，网关与 ESP32 都可自动重连
3. 重启 ESP32 设备后，网关可再次识别设备并恢复命令下发
4. 恢复后的 `get_status` / `set_led` 验证通过
5. 恢复后的 OTA 模拟复验通过

---

## 12. 联调命令附录

### 12.1 启动

Broker：

```bash
nohup mosquitto -c ~/linux_project_directory/proj2/serial-gateway/config/mosquitto_1884.conf -v >/tmp/mosquitto_1884.log 2>&1 &
```

确认 Broker 已启动：

```bash
pgrep -a mosquitto
ss -ltnp | rg ':1884\b'
tail -n 50 /tmp/mosquitto_1884.log
```

说明：

1. 当前联调使用 [mosquitto_1884.conf](../../serial-gateway/config/mosquitto_1884.conf)
2. 配置监听 `0.0.0.0:1884`
3. 当前为开发联调配置，`allow_anonymous true`
4. 局域网内 ESP32 可直接连接 `192.168.1.102:1884`

网关：

```bash
cd ~/linux_project_directory/proj2/serial-gateway
pkill -f serial_gateway
./build/serial_gateway --config=config/gateway.yaml
```

MQTT 观察：

```bash
mosquitto_sub -h 127.0.0.1 -p 1884 -t 'gateway/#' -v
```

### 12.2 设备状态

```bash
curl -s http://127.0.0.1:9010/api/devices
curl -s http://127.0.0.1:9010/api/commands/recent
```

### 12.3 命令下发

开灯：

```bash
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"set_led","on":true}'
```

关灯：

```bash
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"set_led","on":false}'
```

查状态：

```bash
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"get_status"}'
```

设置上报周期：

```bash
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"set_report_interval","interval_ms":3000}'
```

设置日志级别：

```bash
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"set_log_level","level":"DEBUG"}'
```

说明：

1. `set_report_interval` 为当前 MQTT 无线节点已验证支持命令
2. `set_log_level` 为兼容保留命令，当前设备侧返回成功 ACK，但逻辑为 `noop`

### 12.4 OTA 模拟

创建 OTA 任务：

```bash
curl -s -X POST http://127.0.0.1:9010/api/ota/tasks \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": 2,
    "device_type": "esp32-wifi-node",
    "firmware_id": "esp32-wifi-node-1.1.0",
    "transport": "mqtt",
    "firmware_url": "http://example.com/fake.bin"
  }'
```

查看任务列表：

```bash
curl -s http://127.0.0.1:9010/api/ota/tasks
```

查看单个任务：

```bash
curl -s http://127.0.0.1:9010/api/ota/tasks/ota-1782723717552
curl -s http://127.0.0.1:9010/api/ota/tasks/ota-1782723717552/events
```

### 12.5 直接查看 OTA 数据库

注意：当前 OTA 任务库实际为 SQLite 数据库，文件名应使用 `.db`。

查看最近任务：

```bash
sqlite3 ~/linux_project_directory/proj2/serial-gateway/data/ota_tasks.db \
'select task_uuid,device_id,device_type,firmware_id,state,last_error from ota_tasks order by rowid desc limit 10;'
```

查看单个任务事件：

```bash
sqlite3 ~/linux_project_directory/proj2/serial-gateway/data/ota_tasks.db \
"select task_uuid,ts_unix_ms,state,detail from ota_events where task_uuid='ota-1782723717552' order by id;"
```

---

## 13. 恢复验收

### 13.1 目标

验证以下场景下链路可自动恢复：

1. 重启 `serial-gateway`
2. 重启 MQTT Broker
3. 重启 ESP32 设备
4. 恢复后命令与 OTA 仍可正常工作

### 13.2 基线确认

```bash
curl -s http://127.0.0.1:9010/api/devices
mosquitto_sub -h 127.0.0.1 -p 1884 -t 'gateway/#' -v
curl -s http://127.0.0.1:9010/api/ota/tasks
```

期望：

1. `device_id=2` 在线
2. `active_transport` 为 `mqtt`
3. 能持续看到 MQTT 上行消息

### 13.3 重启网关

```bash
pkill -f serial_gateway
cd ~/linux_project_directory/proj2/serial-gateway
./build/serial_gateway --config=config/gateway.yaml
```

恢复后验证：

```bash
curl -s http://127.0.0.1:9010/api/devices
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"get_status"}'
```

### 13.4 重启 Broker

```bash
pkill mosquitto
nohup mosquitto -c ~/linux_project_directory/proj2/serial-gateway/config/mosquitto_1884.conf -v >/tmp/mosquitto_1884.log 2>&1 &
```

恢复后验证：

```bash
pgrep -a mosquitto
ss -ltnp | rg ':1884\b'
curl -s http://127.0.0.1:9010/api/devices
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"set_led","on":true}'
```

### 13.5 重启设备

重启 ESP32 或重新上电后，验证：

```bash
curl -s http://127.0.0.1:9010/api/devices
curl -s -X POST http://127.0.0.1:9010/api/command \
  -H 'Content-Type: application/json' \
  -d '{"device_id":2,"command_type":"get_status"}'
```

### 13.6 恢复后 OTA 模拟

```bash
curl -s -X POST http://127.0.0.1:9010/api/ota/tasks \
  -H 'Content-Type: application/json' \
  -d '{
    "device_id": 2,
    "device_type": "esp32-wifi-node",
    "firmware_id": "esp32-wifi-node-1.1.0",
    "transport": "mqtt",
    "firmware_url": "http://example.com/fake.bin"
  }'
```

### 13.7 通过标准

满足以下条件视为恢复验收通过：

1. 重启网关后设备自动恢复在线
2. 重启 Broker 后网关 MQTT ingress 与设备都能自动重连
3. 重启设备后网关能再次识别设备并下发命令
4. 恢复后的 `get_status` / `set_led` 仍然成功
5. 恢复后 OTA 模拟任务仍可成功创建并完成

当前状态：以上恢复验收项均已完成并通过。

---

## 14. 结论

当前可以认为：

1. `esp32_node_new` 的 MQTT 改造已完成
2. `serial-gateway` 对 MQTT 原生设备的接入、命令、OTA 模拟主链路已完成并实测通过
3. `serial-gateway` 对 MQTT 原生设备的恢复能力已完成实测验证并通过
4. 当前状态已达到可验收、可交付条件
5. 后续工作重点应转为代码加固、更多命令覆盖和长期稳定性观察
