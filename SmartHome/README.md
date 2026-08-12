# STM32F407 SmartHome Firmware

该目录包含智能家居 STM32F407 应用固件和 A/B Bootloader。应用基于 FreeRTOS 组织传感器采集、告警、显示、执行器、外部 Flash 存储、网关通信和任务看护；Bootloader 负责非活动分区写入、镜像校验、试启动、健康确认与失败回滚。

## 目录

- `1_App/`：FreeRTOS 业务任务、网关协议、离线缓存和设备控制。
- `2_Device/`：面向业务层的设备抽象。
- `3_Protocol/`：MQTT 客户端与报文协议组件。
- `4_MiddleWare/`：FreeRTOS 内核及 F407 使用的 Cortex-M4F 移植层。
- `5_Platform/`：传感器、显示、存储、网络等平台适配。
- `6_ModuleDrives/`：DHT11、光照、OLED、W25Qxx 等驱动。
- `7_HAL/`：STM32F407 工程所需 CMSIS 与 HAL 依赖。
- `8_Core/`：板级配置、中断、系统时钟与故障诊断。
- `bootloader/`：A/B Bootloader、Metadata 和 OTA 协议实现。
- `Project/`：Keil F407 应用 A/B 槽及 Bootloader 工程。

## 构建入口

- 应用：`Project/SmartHome_F407.uvprojx`
- Bootloader：`Project/Bootloader_F407.uvprojx`
- 分区脚本：`Project/smarthome_f407_app_a.sct`、`Project/smarthome_f407_app_b.sct`、`Project/bootloader_f407.sct`

现场 MQTT/Wi-Fi 配置不要直接写入公共文件。复制 `config_private_template.h` 为 `config_private.h` 后填写；后者已被 Git 忽略。

升级流程、构建与验收说明见 [README_Upgrade_CN.md](README_Upgrade_CN.md)、[TEST_PLAN.md](TEST_PLAN.md) 和 [bootloader/README.md](bootloader/README.md)。

