# SmartHome 升级说明（中文）

## 1. 项目定位

本项目从课程式 Demo 升级为可用于求职展示的嵌入式项目：

`STM32F103 + FreeRTOS + ESP8266 + MQTT`

目标：实现“智能家居监测与告警终端”，具备本地采集、状态显示、云端控制与告警能力。

## 2. 当前任务架构

在 `1_App/main.c` 中注册：

1. `GatewayTask`（优先级 10）：串口协议收发（对接 serial-gateway）
2. `AlarmTask`（优先级 4）：阈值判断与自动模式控制
3. `SensorTask`（优先级 3）：周期采集（当前为 stub）
4. `DisplayTask`（优先级 2）：周期显示（当前串口模拟）
5. `KeyTask`（优先级 2）：按键上报与长按切换模式
6. `LedTask`（优先级 1）：执行 LED 控制

## 3. 核心能力

1. 统一状态管理：`smarthome_state.h/.c`
2. 集中配置管理：`config.h/.c`
3. 分级日志：`log.h`
4. 串口二进制协议（`AA55|LEN|TYPE|PAYLOAD|CRC16`）
5. 自动/手动双模式
6. 阈值告警与状态上报
7. 命令执行 ACK（`COMMAND_REQ -> COMMAND_ACK`）

## 4. 历史 MQTT Topic（兼容代码仍保留）

定义于 `config.h`：

1. 下行命令：`SH_TOPIC_CMD`
2. 状态上报：`SH_TOPIC_STATUS`
3. 按键事件：`SH_TOPIC_KEY_EVENT`
4. 告警事件：`SH_TOPIC_ALARM`

## 5. Gateway 串口协议关键字段

`SENSOR_DATA(0x01)` 负载：

- `device_id`
- `temperature_x10`（`int16`）
- `humidity_x10`（`uint16`）
- `voltage_mv`（`uint16`，当前固定 3300）
- `status`（bit0:led, bit1:alarm, bit2:sensor_valid, bit3:auto_mode）

支持下行 `COMMAND_REQ(0x10)` 命令：

1. `0x01 set_led`（arg: `on`）
2. `0x02 set_buzzer`（arg: `on`）
3. `0x03 set_mode`（arg: `mode`，`0=manual/1=auto`）
4. `0x04 get_status`（无参数）
5. `0x05 set_threshold`（arg: `temp_x10`,`humi_x10`）
6. `0x06 set_log_level`（arg: `0..2`）

设备执行后返回 `COMMAND_ACK(0x04)`：

- `device_id`
- `command_id`（小端）
- `result`（`0=OK,1=BAD_PAYLOAD,2=UNKNOWN_CMD`）

## 6. 联调建议

1. 先按 `TEST_PLAN.md` 完整走一遍。
2. 测试报告按 `TEST_REPORT_TEMPLATE.md` 填写。
3. 面试讲解优先围绕：任务分层、状态机、协议设计、重连策略。

## APP_A / APP_B 构建方法（STM32F407）

- APP_A（槽位 A，链接到 `0x08020000`）
  - `UV4.exe -b Project/SmartHome_F407.uvprojx -t SmartHome_F407`
- APP_B（槽位 B，链接到 `0x08080000`）
  - `powershell -ExecutionPolicy Bypass -File scripts/build_f407_app_b.ps1`
  - 或 `UV4.exe -b Project/SmartHome_F407.uvprojx -t SmartHome_F407_B`

注意：APP_A / APP_B 镜像是 slot-specific image，不能混用烧录槽位。
