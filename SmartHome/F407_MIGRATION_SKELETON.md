# F407 Migration Skeleton

## What Is Already Abstracted

1. MCU profile switch:
- File: `8_Core/board_profile.h`
- Default profile: `SH_MCU_F103`
- Migration profile: `SH_MCU_F407`

2. Board pin/UART mapping:
- File: `8_Core/board_pins.h`
- Encapsulates debug/net/gateway UART, LED/KEY pins, IRQ aliases.

3. Main clock config split:
- File: `1_App/main.c`
- `SystemClock_Config()` now has `F103` and `F407` branches.

4. Driver include decoupling:
- Drivers now include `main.h`/board profile instead of hard-coded `stm32f1xx_hal.h`.

## Current Limitation (Expected)

This repository still uses F1 startup/CMSIS/HAL/Keil target files by default.
`SH_MCU_F407` switch is a migration skeleton, not a fully linked F407 build yet.

## Next Steps When F407 Board Arrives

1. Add STM32F4 HAL/CMSIS pack into project tree (or Keil pack path).
2. Create Keil target `SmartHome_F407`:
- Device: STM32F407xx
- Startup file: `startup_stm32f407xx.s`
- System file: `system_stm32f4xx.c`
- HAL config: `stm32f4xx_hal_conf.h`
- Define macro: `SH_MCU_F407`

3. Review board-specific pin mapping in `8_Core/board_pins.h`:
- Debug UART (ST-Link VCP mapping)
- ESP8266 UART mapping
- Gateway UART mapping
- LED/KEY pin mapping and EXTI line

4. Validate clock tree for actual HSE frequency in `SystemClock_Config()` F407 branch.
5. Run flash and UART capture scripts with `--mcu f407`.

## Script Usage (F407 Path)

```bash
./scripts/flash_firmware.sh --method openocd --mcu f407
./scripts/planA_onekey.sh --use-existing --mcu f407 --skip-capture
```
