# STM32 over ESP32 WiFi 通信适配

- 架构：STM32 USART3 <-> ESP32 UART2 <-> tcp_binary(9101) <-> serial-gateway
- ESP32 只做 AA55 帧感知转发，不处理业务 ACK
- Gateway 保留 serial/wifi 链路，新增 tcp_binary 高优先级链路
- active transport 优先级：tcp_binary > wifi > serial
- tcp_binary 心跳：TYPE=0x7E，超时按 `heartbeat_timeout_ms` 断开
