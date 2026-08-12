# 发布检查清单

## 一、发布前验证

- [ ] `ctest` 在本机构建目录通过
- [ ] `./tests/smoke.sh` 通过（TCP 上报链路）
- [ ] `./tests/recovery.sh` 通过（缓存补发）
- [ ] `./tests/command_smoke.sh` 通过（下行命令）
- [ ] `./tests/heartbeat_smoke.sh` 通过（网关心跳）
- [ ] `./tests/reload_smoke.sh` 通过（热加载）
- [ ] `./tests/monitor_smoke.sh` 通过（监控接口）
- [ ] `./tests/multi_instance_smoke.sh` 通过（多实例）
- [ ] `./tests/mqtt_smoke.sh` 通过或明确标注跳过原因
- [ ] ARM 交叉构建完成（`build-arm`）
- [ ] QEMU ARM 验证完成或明确标注可选

## 二、WiFi 第二节点专项（当前项目重点）

- [ ] `wifi_device_server` 已启用并监听 `9100`
- [ ] `device_id=2`（`wifi-node-02`）可在 `/api/devices` 看到 `online=true`
- [ ] `link_type=wifi`、`wifi_connected=true`、`wifi_rssi` 有值
- [ ] `command_sender` 对 WiFi 设备使用 `RAW JSON` 下发命令
- [ ] `command_ack` 能回收，`result=0` 正常
- [ ] metrics 包含：
  - [ ] `sg_wifi_clients_connected`
  - [ ] `sg_wifi_devices_online`
  - [ ] `sg_device_online{device_id="2",...}`

## 三、打包产物

- [ ] 源码压缩包已生成
- [ ] `x86_64` 运行包已生成
- [ ] `armhf` 运行包已生成（如本次发布包含 ARM）
- [ ] `SHA256SUMS` 已生成
- [ ] 运行包包含 `config/systemd/scripts/docs`

## 四、文档检查

- [ ] `README.md` 链接和目录有效
- [ ] `CHANGELOG.md` 已更新本次变更
- [ ] `docs/release_notes.md` 已更新本次变更
- [ ] `docs/demo_commands.md` 命令可直接执行
- [ ] `docs/deployment.md` 与当前端口配置一致（9000/9001/9010/9100）
- [ ] `docs/protocol.md` 已包含 WiFi JSON 命令说明

## 五、发布动作

- [ ] 创建 git tag（例如 `v1.0.0`）
- [ ] 上传 release 资产
- [ ] 发布摘要（包含已知限制与回滚方案）
