# STM32 OTA 强化项落地说明

## 1. metadata 原子提交（append-only journal）
- metadata 区仍在 `0x08010000`（Sector4，64KB）。
- 采用 journal 追加写：每次 commit 追加一条完整 metadata 记录（CRC/magic/schema 校验）。
- 启动时扫描 journal，按 `meta_seq` 选最新有效记录。
- journal 写满时才触发 GC：整扇区擦除后写入最新记录，再继续追加。

## 2. 防回滚（无签名版最小闭环）
- manifest 增加 `image_version`（u32）。
- 打包脚本将 `x.y.z` 转为版本码：`(x<<16) | (y<<8) | z`。
- 网关 PREPARE 发送 `image_version`。
- bootloader PREPARE 校验：
  - `image_version < min_allowed_version` 时 NACK。
- APP confirm 成功后推进 `min_allowed_version` 到当前已确认版本，形成防回滚闭环。
- NACK 新增错误码：
  - `10`: rollback blocked (`BL_ERR_ROLLBACK`)

## 3. 故障注入自动化报告
- 新增执行脚本：`scripts/run_stm32_fault_matrix.sh`
- 新增报告生成器：`scripts/gen_fault_report.py`
- 默认矩阵：
  - `success_baseline`
  - `fault_verify_fail`
  - `fault_disconnect_seq3`
  - `fault_nack_seq2`
- `fault_disconnect_seq3` 判定要求：必须进入 `FAILED/CANCELED` 终态（非 `NOT_SUCCESS` 宽松条件）。

## 4. 一键执行
```bash
cd serial-gateway
cmake -S . -B build && cmake --build build -j
./scripts/run_stm32_fault_matrix.sh ./tmp_fault_matrix
cat ./tmp_fault_matrix/report.md
```
