# ESP32 Comm Adapter

STM32 USART3 <-> ESP32 UART2 <-> WiFi TCP Binary <-> Linux Gateway。

- 轻量帧感知：识别 `AA55|LEN|TYPE|PAYLOAD|CRC16`
- 不做业务解析，不生成业务 ACK，不修改 payload
- 仅完整合法帧进入 TCP 发送

## 配置
- 仓库默认 `config.h` 只保留占位值。
- 现场 WiFi、密码、网关地址请写在 `config_local.h` 中覆盖，不要把真实凭据提交回仓库。
