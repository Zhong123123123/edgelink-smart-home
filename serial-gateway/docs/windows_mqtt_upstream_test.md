# 网关上行 MQTT 验收说明

## 1. 目标

将 `serial_gateway` 的上行链路从原来的 TCP 上传切换为 MQTT 上传，并在 Windows 电脑上作为“上游”完成订阅验收。

当前版本先覆盖两类上报：

- 数据上报：`upstream/gw001/data`
- 网关心跳：`upstream/gw001/heartbeat`

本次不包含独立的 `event` / `status` / `command_ack` 专用上游 topic 设计，这些内容如果需要，再作为下一阶段扩展。

## 2. 当前配置

当前仓库默认配置文件 [gateway.yaml](../config/gateway.yaml) 已切换为 MQTT 上报：

```yaml
uploader:
  type: "mqtt"
  host: "127.0.0.1"
  port: 1884
  mqtt_topic: "upstream/gw001/data"
  mqtt_heartbeat_topic: "upstream/gw001/heartbeat"
  mqtt_client_id: "serial-gateway-gw001"
```

说明：

- `mqtt_topic` 用于上传采集数据
- `mqtt_heartbeat_topic` 用于上传网关心跳
- 如果不显式配置 `mqtt_heartbeat_topic`，程序会退化为 `mqtt_topic + "/heartbeat"`

## 3. Linux 本机 Broker 验收

### 3.1 启动 Broker

仓库里已经有一个允许局域网访问的 Broker 配置：

```bash
mosquitto -c serial-gateway/config/mosquitto_1884.conf -v
```

默认监听：

- `0.0.0.0:1884`

### 3.2 启动网关

```bash
cd serial-gateway
./build/serial_gateway --config=config/gateway.yaml
```

### 3.3 本机订阅验证

另开终端：

```bash
mosquitto_sub -h 127.0.0.1 -p 1884 -t "upstream/#" -v
```

预期看到两类消息：

```text
upstream/gw001/heartbeat {...}
upstream/gw001/data {...}
```

其中：

- `heartbeat` 周期由 `heartbeat.interval_sec` 控制
- `data` 只有在网关采集到设备数据后才会出现

## 4. Windows 电脑作为上游验收

如果你准备把 Windows 电脑当成“服务器侧 MQTT Broker”，做法如下。

### 4.1 在 Windows 启动 Broker

可直接安装 Mosquitto，然后启动默认 1883 端口。

如果只是临时验收，Windows 侧只要能提供一个 MQTT Broker 即可。

### 4.2 修改网关上传目标

把 [gateway.yaml](../config/gateway.yaml) 里的 `uploader.host` 和 `uploader.port` 改成 Windows 电脑地址，例如：

```yaml
uploader:
  type: "mqtt"
  host: "192.168.1.50"
  port: 1883
  mqtt_topic: "upstream/gw001/data"
  mqtt_heartbeat_topic: "upstream/gw001/heartbeat"
  mqtt_client_id: "serial-gateway-gw001"
```

### 4.3 Windows 侧订阅

在 Windows 上执行：

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -t "upstream/#" -v
```

如果 Broker 跑在 Windows 本机，这里的 `-h` 用 `127.0.0.1`。
如果你是远程连接其他 Broker，则替换成对应 IP。

### 4.4 启动 Linux 网关

```bash
cd serial-gateway
./build/serial_gateway --config=config/gateway.yaml
```

### 4.5 预期结果

Windows 订阅端应看到：

- `upstream/gw001/heartbeat`
- `upstream/gw001/data`

如果设备在线且持续上报，`data` 应连续出现。

## 5. 故障恢复验收

建议至少补一次 Broker 重启恢复测试。

步骤：

1. 保持 `serial_gateway` 运行
2. 停掉 Broker
3. 观察网关日志中的 MQTT 断连与重连信息
4. 重新启动 Broker
5. 继续订阅 `upstream/#`
6. 确认心跳和数据恢复上报

预期：

- 网关进程不退出
- Broker 恢复后自动重连
- 恢复后继续收到 `heartbeat` 和 `data`

## 6. 回退方案

如果要回到原来的 TCP 上传模式，把 [gateway.yaml](../config/gateway.yaml) 的 `uploader` 改回：

```yaml
uploader:
  type: "tcp"
  host: "127.0.0.1"
  port: 9000
```

其余 `mqtt_*` 字段可保留不生效，也可以删除。

## 7. 当前结论

当前仓库已经具备：

- 网关采集数据通过 MQTT 上报
- 网关心跳通过 MQTT 上报
- 面向 Windows 电脑作为上游 Broker 的联调路径
- 发生 Broker 断开后的自动恢复能力

如果后续要继续扩展，上一个最自然的方向是：

- 增加独立的 `upstream/gw001/event`
- 增加设备状态聚合 topic
- 增加 command/ota 执行结果的专用上游主题
