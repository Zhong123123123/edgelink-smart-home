# SmartHome F407 A/B + Bootloader：变更清单与回归测试清单

## 1. 变更范围（仅基础设施，不改业务逻辑）

本次变更只涉及 Keil 工程配置、scatter、构建脚本、bootloader 启动链路与诊断日志。  
未修改 `1_App`、`2_Device`、`3_Network` 等业务模块逻辑。

---

## 2. 代码与工程变更清单

## 2.1 Keil 工程与链接

1. `Project/SmartHome_F407.uvprojx`
   - 新增 target：`SmartHome_F407_B`
   - 复制 `SmartHome_F407` 编译配置（源文件、宏、头文件路径、编译选项）
   - `SmartHome_F407_B` scatter 指向 `.\smarthome_f407_app_b.sct`
   - 保持 `SmartHome_F407` 继续作为 APP_A

2. `Project/smarthome_f407_app_a.sct`
   - `IROM1 = 0x08020000, size = 0x00060000`
   - `IRAM1 = 0x20000000, size = 0x00020000`

3. `Project/smarthome_f407_app_b.sct`
   - `IROM1 = 0x08080000, size = 0x00060000`
   - `IRAM1 = 0x20000000, size = 0x00020000`

## 2.2 构建脚本

1. `scripts/build_f407_app_b.ps1`
   - 调用：`UV4.exe -b Project/SmartHome_F407.uvprojx -t SmartHome_F407_B`
   - 构建失败退出非 0
   - 成功输出产物路径（`Project/Objects/SmartHome_F407_B.axf`）

## 2.3 Bootloader 跳转稳定性修复

1. `bootloader/bootloader_main.c`
   - 修复点：避免 `__set_MSP` 后继续使用 C 栈局部变量导致跳转地址损坏
   - 实现：`noreturn + naked` 跳转入口，`MSR MSP` 后立即 `BX`

## 2.4 Bootloader 串口日志与诊断

1. `bootloader/bootloader_config.h`
   - 新增日志开关与参数（默认 `USART1 PA9 TX, 115200 8N1`）

2. `bootloader/bootloader_main.c`
   - 新增启动日志：
     - Boot start / RCC CSR
     - metadata valid/invalid
     - metadata 关键字段
     - target
     - vector 检查结果（SP/PC）
     - jump 参数

3. `bootloader/bootloader_metadata.c/.h`
   - 增加 `g_bl_meta_validate_reason` 与失败码
   - 可定位 metadata 无效根因（magic/schema/crc）

## 2.5 Metadata 自愈初始化（关键）

1. `bootloader/bootloader_main.c`
   - 当 metadata 校验失败时：
     - 生成默认 metadata
     - 擦除 `0x08010000`（Sector 4）
     - 写回默认 metadata
     - 复验并打印结果
   - 目标：后续不再依赖“每次全片擦除”

---

## 3. 已验证结果（本次实测）

1. A 路径日志闭环（通过）
   - `metadata primary valid`
   - `target=0x08020000`
   - `check vector ... sp=0x20003A08 pc=0x08020309`
   - `jump base=0x08020000 ...`

2. B 路径日志闭环（通过，切槽验证）
   - `target=0x08080000`
   - `check vector ... sp=0x20003A08 pc=0x08080309`
   - `jump base=0x08080000 ...`

3. metadata 无效原因定位（通过）
   - 失败码 `0x00000002`（magic 不匹配）
   - 自愈后恢复为 `metadata primary valid`

---

## 4. 回归测试清单（可重复执行）

## 4.1 构建回归

1. Build `SmartHome_F407`（APP_A）成功  
2. Build `SmartHome_F407_B`（APP_B）成功  
3. 产物存在：
   - `Project/Objects/SmartHome_F407.axf`
   - `Project/Objects/SmartHome_F407_B.axf`

## 4.2 链接地址回归（map 真值）

1. A map：`LR_IROM1 Base = 0x08020000`  
2. B map：`LR_IROM1 Base = 0x08080000`

## 4.3 启动链路回归

1. 正常上电（默认 metadata）应进 A：
   - 串口 `target=0x08020000`
2. 切槽到 B（metadata 或调试注入）应进 B：
   - 串口 `target=0x08080000`

## 4.4 向量合法性回归

1. `0x08020000/04`：
   - SP 属于 `0x200xxxxx`
   - PC 属于 `0x0802xxxx` 且 Thumb 位有效
2. `0x08080000/04`：
   - SP 属于 `0x200xxxxx`
   - PC 属于 `0x0808xxxx` 且 Thumb 位有效

## 4.5 Metadata 自愈回归

1. 人为破坏 metadata magic 后上电
2. 预期日志：
   - `metadata invalid ...`
   - `heal metadata: ...`
   - 再次上电变为 `metadata primary valid`

---

## 5. 风险边界与注意事项

1. A/B 镜像是 slot-specific，禁止混烧槽位  
2. 调试器下“卡在 BX 附近”不直接等于失败，需以脱机冷启动和日志判据为准  
3. 若临时加过 `target = BL_APP_B_BASE` 强制语句，交付前必须删除  
4. metadata 区在 `0x08010000`，后续 OTA 写入需严格遵循同一结构和 CRC 规则

---

## 6. 交付判据（Done Definition）

满足以下全部条件即视为完成：

1. A/B target 均可稳定构建  
2. A/B map 链接地址分别为 `0x08020000` / `0x08080000`  
3. Bootloader 能按 metadata 选择目标槽并完成跳转  
4. metadata 异常时可自动自愈并恢复 `metadata primary valid`  
5. 全流程无需每次全片擦除

