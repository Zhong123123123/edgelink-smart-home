# STM32F407 Bootloader 跳转失败复盘（2026-05-11）

## 1. 目标与约束
- 目标：Bootloader 从 `0x08000000` 跳转到 APP `0x08020000`，APP 正常运行。
- 固定约束：
  - Bootloader 分区：`0x08000000 / 0x10000`
  - APP 分区：`0x08020000 / 0xE0000`
  - 不改 OTA 协议
  - 不改分区方案

## 2. 现场现象
- 反复出现：上电后“短闪若干次后常亮/无串口输出/无 OLED 反应”。
- 诊断阶段曾出现稳定序列：
  - `2短 + 7长 + 常亮`
  - `2短 + 7长 + 5闪`
  - `2短 + 6长 + 常亮一段 + 5闪`
  - `2短 + 5长 + 5闪`

## 3. 已确认事实（关键证据）
- Bootloader 可独立启动（PF9 最小闪灯测试通过）。
- APP 镜像确实放在 `0x08020000`（hex 首地址、vector 内容均检查过）。
- Bootloader 读取到的 APP 向量表值一度有效：
  - SP 在 SRAM 合法范围
  - PC 在 Flash 合法范围，且 Thumb bit0=1
- APP 与 Bootloader 均曾进入各自 HardFault 诊断路径：
  - APP 侧闪码 `5` 对应 `HFSR.VECTTBL`（向量取值异常）
  - Bootloader 侧在不同阶段（5/6/7）均观察过异常迹象

## 4. 本次排查做过的动作
- 做过的方向：
  - VTOR 设置时机与位置（SystemInit/main/Bootloader jump）
  - jump 序列（disable irq、systick、nvic 清理、MSP、barrier）
  - HardFault 捕获（HFSR/CFSR/MMFAR/BFAR/stacked PC/LR/xPSR）
  - LED 分阶段闪码诊断（Bootloader 与 APP 两侧）
  - startup 栈大小试探（`0x400 -> 0x2000`）
- 最终处理：
  - 已按你的要求把核心文件恢复到桌面备份版本（逐文件 `fc` 对比确认一致）。

## 5. 为什么这次“看起来一直绕圈”
- 根本原因不是单点 bug，而是“多因素叠加 + 现象重叠”：
  - Bootloader/APP 两侧都可能 fault，灯效容易混淆；
  - 某些调试版本在不同阶段都能触发异常，导致症状迁移；
  - 你的业务工程里还有传感器缺失（DHT 未接）带来的“业务无响应”假象，掩盖了启动链路问题。
- 结果：同一块板子上出现“跳转问题 + 业务传感器问题”叠加，体感上像“全坏了”。

## 6. 当前状态（截至复盘时）
- 已恢复备份一致文件：
  - `1_App/main.c`
  - `bootloader/bootloader_main.c`
  - `8_Core/system_stm32f4xx.c`
  - `8_Core/stm32f4xx_it.c`
- 额外说明：
  - `Project/startup_stm32f407xx.s` 也已恢复到备份一致。
  - APP 侧已另外补了“DHT 缺失时光敏 fallback”逻辑（仅影响 `app_sensor.c`），用于你当前硬件不接温湿度时仍可观测光敏变化。

## 7. 后续建议（避免再次失控）
1. 先冻结“可运行基线”  
   - 基线定义：能进业务、OLED 正常、串口有日志。  
   - 对基线打 tag/拷贝，后续每次只改一处。
2. 启动链路与业务链路分离验证  
   - 先验证 jump（最小 APP）；
   - 再接入完整 SmartHome 业务。
3. 每轮只改一个变量  
   - 例如只改 VTOR，或只改 NVIC 清理，不混改。
4. 用固定观察矩阵记录  
   - 烧录顺序、灯序列、PC 地址、HFSR/CFSR 同步记录，避免“记忆串线”。

## 8. 结论
- 本次失败本质是“Bootloader->APP 跳转不稳定 + 诊断期多版本叠加 + 业务传感器未接导致的假故障”共同造成。
- 已完成回退到你备份基线，并保留了可继续排查的文档化结论。

