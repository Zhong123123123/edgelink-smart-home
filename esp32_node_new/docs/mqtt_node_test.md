# ESP32 节点 MQTT 联调测试

## 1. 准备 Broker

在 Ubuntu 上安装 Mosquitto：

```bash
sudo apt install mosquitto mosquitto-clients
```

启动本地 Broker：

```bash
mosquitto -v
```

## 2. 配置节点

将 `esp32_node/config.h.example` 复制为 `esp32_node/config.h`，并至少配置以下参数：

1. `WIFI_SSID`
2. `WIFI_PASSWORD`
3. `MQTT_HOST`
4. `MQTT_PORT`
5. `MQTT_CLIENT_ID`
6. `GATEWAY_ID`
7. `ACTIVE_COMM_MODE COMM_MODE_MQTT`

默认 MQTT topic 如下：

1. `gateway/{gateway_id}/device/{device_id}/telemetry`
2. `gateway/{gateway_id}/device/{device_id}/status`
3. `gateway/{gateway_id}/device/{device_id}/event`
4. `gateway/{gateway_id}/device/{device_id}/command/down`
5. `gateway/{gateway_id}/device/{device_id}/command/ack`
6. `gateway/{gateway_id}/device/{device_id}/ota/start`
7. `gateway/{gateway_id}/device/{device_id}/ota/status`

## 3. 订阅消息

订阅节点所有 MQTT 消息：

```bash
mosquitto_sub -h 127.0.0.1 -t "gateway/#" -v
```

## 4. 下发命令

第二阶段推荐优先使用 `cmd` 作为主命令字段，旧的 `command_type` 仍兼容。

打开 LED：

```bash
mosquitto_pub -h 127.0.0.1 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1001,"cmd":"led_set","value":1}'
```

修改上报周期：

```bash
mosquitto_pub -h 127.0.0.1 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1002,"cmd":"report_interval_set","interval_ms":3000}'
```

主动获取状态：

```bash
mosquitto_pub -h 127.0.0.1 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1003,"cmd":"status_get"}'
```

仍兼容旧的 `command_type` 形式：

```bash
mosquitto_pub -h 127.0.0.1 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"type":"command","command_id":1004,"command_type":"set_led","value":0}'
```

第二阶段 OTA 启动推荐走专用 topic：

```bash
mosquitto_pub -h 127.0.0.1 \
  -t "gateway/gw001/device/2/ota/start" \
  -m '{"command_id":4001,"version":"1.0.1","firmware_url":"http://example.com/fake.bin","size":123456,"crc32":"1234ABCD"}'
```

## 5. 预期结果

1. 串口日志中出现 `WiFi connected` 和 `MQTT connected`
2. `mosquitto_sub` 能收到 `telemetry`、`status`、`event`、`command ack`，OTA 场景下还能收到 `ota/status`
3. `set_led` 能改变 RGB 覆盖状态，并返回命令 ACK
4. `set_report_interval` 能修改 telemetry 上报周期
5. WiFi 或 Broker 断开后，节点会自动重连
6. 重连后，节点会重新订阅 `command/down` 并继续处理命令

## 6. 说明

1. 当前 `PubSubClient` 发布路径使用的是 QoS 0
2. 在线和离线状态通过 retained 的 `status` topic 表达
3. 仍然保留 TCP JSON 回退能力，可通过 `ACTIVE_COMM_MODE COMM_MODE_TCP_JSON` 切回
4. 第二阶段推荐使用 `cmd` 与专用 `ota/*` topic，但保留第一阶段字段兼容

## 7. 已验证联调记录

本次实际联调使用的环境如下：

1. Broker 地址：`192.168.1.102`
2. Broker 端口：`1884`
3. Gateway ID：`gw001`
4. Device ID：`2`
5. 节点模式：`COMM_MODE_MQTT`
6. 传感器模式：模拟模式

Broker 启动方式示例（这是第一步，在网关这边运行的）：

```bash
cat >/tmp/mosq-esp32.conf <<'EOF'
listener 1884 0.0.0.0
allow_anonymous true
EOF

mosquitto -c /tmp/mosq-esp32.conf -v
```

订阅命令示例（这是第二步，也是在网关这边运行的，要新开一个终端）：

```bash
mosquitto_sub -h 127.0.0.1 -p 1884 -t "gateway/#" -v
```

### 7.1 标准命令验证（再新开一个终端）：

验证 `set_led`：

```bash
mosquitto_pub -h 127.0.0.1 -p 1884 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1001,"cmd":"led_set","value":1}'
```

观察到的 ACK：

```text
gateway/gw001/device/2/command/down {"type":"command","command_id":1001,"command_type":"set_led","value":1}
gateway/gw001/device/2/command/ack {"device_id":2,"device_name":"wifi-node-02","event_type":"command_ack","timestamp":0,"command_id":1001,"command_result":0,"payload_summary":"cmd_id=1001,result=0","link_type":"wifi","wifi_rssi":-32,"seq":1060}
```

验证 `get_status`：

```bash
mosquitto_pub -h 127.0.0.1 -p 1884 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1002,"cmd":"status_get"}'
```

观察到的 ACK：

```text
gateway/gw001/device/2/command/down {"type":"command","command_id":1002,"command_type":"get_status"}
gateway/gw001/device/2/command/ack {"device_id":2,"device_name":"wifi-node-02","event_type":"command_ack","timestamp":0,"command_id":1002,"command_result":0,"payload_summary":"cmd_id=1002,result=0","link_type":"wifi","wifi_rssi":-27,"seq":1104}
```

验证 `set_report_interval`：

```bash
mosquitto_pub -h 127.0.0.1 -p 1884 \
  -t "gateway/gw001/device/2/command/down" \
  -m '{"command_id":1003,"cmd":"report_interval_set","interval_ms":3000}'
```

观察到的 ACK：

```text
gateway/gw001/device/2/command/down {"type":"command","command_id":1003,"command_type":"set_report_interval","interval_ms":3000}
gateway/gw001/device/2/command/ack {"device_id":2,"device_name":"wifi-node-02","event_type":"command_ack","timestamp":0,"command_id":1003,"command_result":0,"payload_summary":"cmd_id=1003,result=0","link_type":"wifi","wifi_rssi":-27,"seq":1110}
```

### 7.2 别名命令兼容验证

已验证以下别名命令均可正常工作：

1. `cmd=led_set`
2. `cmd=status_get`
3. `cmd=report_interval_set`

这些 `cmd` 形式已作为第二阶段主语义验证通过，旧的 `command_type` 形式仍兼容。

### 7.3 Broker 断开/恢复重连验证

已验证以下行为：

1. 手动停止 Broker 后，节点不会死机
2. Broker 重新启动后，节点会自动重连
3. 重连后，节点会恢复 `telemetry` 和 `status` 上报
4. 重连后，`command/down` 订阅会自动恢复
5. 重连后再次下发命令，`command/ack` 返回正常

### 7.4 OTA 模拟验证

前提配置：

```cpp
#define ESP32_OTA_REAL 0
```

验证命令：

```bash
mosquitto_pub -h 127.0.0.1 -p 1884 \
  -t "gateway/gw001/device/2/ota/start" \
  -m '{"command_id":4001,"version":"1.0.1","firmware_url":"http://example.com/fake.bin","size":123456,"crc32":"1234ABCD"}'
```

观察结果：

1. 节点正确接收 `ota_start`
2. OTA 状态消息能发布到 `ota/status`，并兼容发布到 `event` topic
3. 模拟 OTA 流程完整执行成功
4. 节点在 OTA 模拟完成后发生重启
5. 重启后 WiFi 和 MQTT 可自动恢复连接

## 8. 当前验证结论

当前已完成验证：

1. MQTT 连接与周期上报：已验证
2. 第二阶段 `cmd` 主命令语义：已验证
3. 第一阶段 `command_type` 兼容：已验证
4. `command/ack` 返回链路：已验证
5. Broker 断开/恢复重连：已验证
6. OTA 专用 topic 与模拟流程及重启恢复：已验证

当前剩余风险：

1. `ESP32_OTA_REAL=1` 时的真实 OTA 下载与写入闭环尚未进行硬件验证

## 9. Linux 网关侧可选桥接

如需由 Linux 网关直接订阅 MQTT 命令并转发给当前 WiFi 节点，可使用新增工具：

```bash
./build/mqtt_command_bridge 127.0.0.1 1884 127.0.0.1 9001
```

参数依次为：

1. MQTT Broker 地址
2. MQTT Broker 端口
3. serial-gateway command socket 地址
4. serial-gateway command socket 端口

该工具会订阅：

1. `gateway/+/device/+/command/down`
2. `gateway/+/device/+/ota/start`

并将收到的 MQTT 控制消息桥接到当前网关已有的 WiFi 节点命令入口。
