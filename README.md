# EdgeLink 智能家居边缘网关与 AI 运维平台

这是一个覆盖 STM32 设备端、ESP32 无线节点与通信桥、Linux 边缘网关、Qt 监控面板和 AI 运维助手的端边一体化工程。项目围绕设备采集与控制、多链路接入、离线容错、STM32/ESP32 OTA、可观测性和智能运维展开。

> 项目定位为个人研发与硬件联调原型。仓库中的 mock、故障注入和多实例工具用于开发验证，不代表生产集群部署规模。

## 系统架构

```text
STM32F407 / ESP32 业务节点
        │  UART / MQTT / TCP JSON
        ▼
ESP32 UART-TCP 桥（可选） ── TCP Binary ──┐
                                          │
                               Linux C++ 边缘网关
                               ├─ 设备注册与命令路由
                               ├─ SQLite 历史与 OTA 状态
                               ├─ HTTP API / Prometheus
                               └─ STM32 / ESP32 OTA
                                  │            │
                         Qt/QML 监控面板   AI Agent 助手
                                          RAG / SQL / HITL
```

## 仓库结构

| 目录 | 作用 | 主要技术 |
| --- | --- | --- |
| `SmartHome/` | STM32F407 业务固件、安全诊断和 A/B Bootloader | C、FreeRTOS、STM32 HAL、UART/DMA、SPI Flash |
| `esp32_node_new/` | ESP32 Wi-Fi 业务节点，负责传感器上报、命令和 HTTP OTA | Arduino、FreeRTOS、MQTT、TCP/JSON |
| `esp32_comm_adapter/` | STM32 UART 与网关 TCP Binary 之间的透明通信桥 | Arduino、FreeRTOS、UART、TCP |
| `serial-gateway/` | Linux 边缘网关、设备管理、持久化、监控及统一 OTA 编排 | C++17、CMake、pthread、SQLite、MQTT、HTTP |
| `gateway-qt-monitor/` | 网关状态、设备、历史数据、命令和 OTA 可视化 | Qt 6、QML、CMake |
| `gateway_ai_assistant/` | 面向设备运维的问答、诊断、审批、报告和批量 OTA 治理 | Python、FastAPI、LangGraph、Streamlit、RAG |

STM32 目录仅纳入 F407 工程实际需要的 CMSIS、HAL 与 FreeRTOS Cortex-M4F 依赖；SDK 中与目标芯片无关的示例、预编译库和其他架构移植层不进入版本库。

## 核心能力

- STM32 端采用 FreeRTOS 管理采集、告警、显示、存储、网关通信和任务看护，并实现离线缓存、ACK 驱动回放、HardFault 快照和复位原因记录。
- ESP32 提供业务节点和 UART-TCP 桥两种角色，使 Wi-Fi 原生设备与传统串口设备都能接入同一网关。
- Linux 网关支持 STM32 直连串口、ESP32 MQTT/TCP JSON、STM32 经 ESP32 桥接三类入口，并统一设备模型、命令和运行指标。
- STM32 OTA 使用双应用分区、镜像校验、试启动、健康确认和失败回滚；ESP32 OTA 使用网关下发、HTTP 下载和状态回传。
- Qt 面板通过 HTTP API 展示运行状态和升级任务；AI 助手以非侵入方式复用网关 API、SQLite 与项目文档。
- AI 助手提供 RAG、受控 SQL、ReAct Tool Calling、只读并行诊断、HITL 审批及批量灰度升级治理。

## 快速开始

各模块可独立构建，具体依赖和硬件接线以模块文档为准：

- [STM32 升级说明](SmartHome/README_Upgrade_CN.md)
- [ESP32 通信桥](esp32_comm_adapter/README.md)
- [Linux 网关](serial-gateway/README.md)
- [Qt 监控端](gateway-qt-monitor/README.md)
- [AI 运维助手](gateway_ai_assistant/README_AI_ASSISTANT.md)

### 1. 配置设备端

仓库不保存真实 Wi-Fi、MQTT 或大模型密钥。

```text
esp32_node_new/config.h.example  -> esp32_node_new/config_local.h
SmartHome/config_private_template.h -> SmartHome/config_private.h
```

`esp32_comm_adapter/config.h` 已提供安全默认值，可通过同目录下的 `config_local.h` 覆盖现场网络配置。

### 2. 构建 Linux 网关

```bash
cmake -S serial-gateway -B serial-gateway/build
cmake --build serial-gateway/build -j
ctest --test-dir serial-gateway/build --output-on-failure
```

### 3. 构建 Qt 监控端

```bash
cmake -S gateway-qt-monitor -B gateway-qt-monitor/build
cmake --build gateway-qt-monitor/build -j
```

### 4. 启动 AI 助手

```bash
python -m pip install -r gateway_ai_assistant/requirements.txt
python -m gateway_ai_assistant.rag.build_index
python -m uvicorn gateway_ai_assistant.app:app --host 127.0.0.1 --port 8010
```

可另行启动 Streamlit 演示端：

```bash
python -m streamlit run gateway_ai_assistant/streamlit_app.py
```

## 安全说明

- 本地配置、运行数据库、日志、固件镜像和构建产物已由 `.gitignore` 排除。
- 示例地址和凭据仅用于说明；部署前应通过本地配置或环境变量覆盖。
- OTA 属于高风险写操作，接入真实设备前请先阅读对应协议、分区和回滚文档并完成板端验证。

## 验证范围

仓库包含 Linux 网关协议、缓存、设备注册和 OTA 状态机测试，以及 AI 助手路由、RAG、SQL、工作流和 Agent 测试。硬件实测记录属于本地联调资料，默认不进入公开仓库。
