# Gateway Qt Monitor

面向联调现场的 Qt 可视化客户端（普通人可读），当前正式界面为 `Qt Widgets` 实现。

## 功能
- 中文指标卡：在线设备、解析帧、ACK 1分钟健康度。
- 设备状态面板：在线/离线、链路类型、上报数量、摘要。
- 最近事件面板：`Seq + Link + Event + T/H/V + Summary`。
- ACK 调试日志面板：直接看 `rx report / tx ack / ack write bytes`。
- 支持“只看异常”过滤。
- 新增控制 Tab：通过 HTTP `POST /api/command` 下发 `get_status`、`set_led`、`set_mode`、`set_threshold`，也可走语义化接口。
- 支持查看最近命令记录：`/api/commands/recent`。
- 也支持语义化接口：`/api/device/{id}/led`、`/api/device/{id}/mode`、`/api/device/{id}/threshold`、`/api/device/{id}/status/query`。
- 新增 OTA Tab：查看 `/api/ota/tasks`，查看任务事件，创建/取消/重试 OTA 任务时需要二次确认。

## 依赖
- Qt6（`Widgets`、`Network`）
- CMake 3.16+

## 构建
```bash
cd gateway-qt-monitor
cmake -S . -B build
cmake --build build -j4
```

## 运行
默认连接 `http://127.0.0.1:9010`：
```bash
./build/gateway_qt_monitor
```

指定网关地址：
```bash
./build/gateway_qt_monitor http://127.0.0.1:8080
```

## 说明
该客户端只读取现有接口，不改动网关协议和 Prometheus：
- `/api/status`
- `/api/devices`
- `/api/recent`
- `/api/commands/recent`
- `/api/debug_ack`
- `/api/ota/tasks`
