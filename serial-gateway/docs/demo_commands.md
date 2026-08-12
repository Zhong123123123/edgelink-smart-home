# 演示命令（可直接复制）

## 1. 编译

```bash
cd serial-gateway
cmake -S . -B build -DSERIAL_GATEWAY_ENABLE_MQTT=ON
cmake --build build -j
```

## 2. 运行基础测试

```bash
ctest --test-dir build --output-on-failure
```

## 3. 端到端链路测试

```bash
./tests/smoke.sh
./tests/recovery.sh
./tests/command_smoke.sh
./tests/heartbeat_smoke.sh
./tests/reload_smoke.sh
./tests/monitor_smoke.sh
```

## 4. 启动核心进程（手动联调）

```bash
mkdir -p logs
nohup ./build/tcp_receiver 9000 > logs/tcp_receiver.log 2>&1 &
nohup sudo ./build/serial_gateway --config=config/gateway.yaml > logs/serial_gateway.log 2>&1 &
```

## 5. 串口设备命令（device_id=1，二进制 DEVCMD）

```bash
./build/command_sender 127.0.0.1 9001 DEVCMD 1 100 2000 set_led 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 101 2000 set_mode 1
./build/command_sender 127.0.0.1 9001 DEVCMD 1 102 2000 set_threshold 35.0 80.0
./build/command_sender 127.0.0.1 9001 DEVCMD 1 103 2000 get_status
./build/command_sender 127.0.0.1 9001 DEVCMD 1 104 2000 set_log_level 2
```

## 6. WiFi 设备命令（device_id=2，必须 RAW JSON）

说明：WiFi 设备不支持 `DEVCMD`，需使用 `RAW`。

```bash
./build/command_sender 127.0.0.1 9001 RAW '{"type":"command","device_id":2,"command_id":10004,"command_type":"get_status","params":{},"timeout_ms":5000}'

./build/command_sender 127.0.0.1 9001 RAW '{"type":"command","device_id":2,"command_id":10005,"command_type":"set_led","params":{"value":1},"timeout_ms":5000}'

./build/command_sender 127.0.0.1 9001 RAW '{"type":"command","device_id":2,"command_id":10006,"command_type":"set_report_interval","params":{"interval_ms":3000},"timeout_ms":5000}'
```

## 7. Monitor / Metrics 查看

```bash
curl -s http://127.0.0.1:9010/api/status | jq
curl -s http://127.0.0.1:9010/api/devices | jq
curl -s http://127.0.0.1:9010/api/recent | jq
curl -s http://127.0.0.1:9010/metrics
```

浏览器页面：

- `http://127.0.0.1:9010/`
- `Recent Samples` 支持按 `device_id` 过滤（`All / 1 / 2`）

## 8. WiFi Mock 联调

```bash
./tools/integration/test_wifi_node.sh
./tools/integration/test_multi_transport.sh
```

## 9. 安装 systemd 服务

```bash
sudo cmake --install build
sudo ./scripts/install_systemd_service.sh
sudo systemctl daemon-reload
sudo systemctl enable serial-gateway.service
sudo systemctl start serial-gateway.service
sudo systemctl status serial-gateway.service
```

## 10. 常用排障

```bash
ps -ef | rg "tcp_receiver|serial_gateway" | rg -v rg
tail -f logs/serial_gateway.log
journalctl -u serial-gateway.service -f
```
