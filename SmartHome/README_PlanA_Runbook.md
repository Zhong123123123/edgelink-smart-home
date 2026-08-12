# Plan A Runbook (Linux)

## 1. Install dependencies (Ubuntu)

```bash
cd SmartHome
./scripts/install_toolchain_ubuntu.sh
```

## 2. Build firmware

Before build, run the delivery preflight:

```bash
./scripts/preflight_check.sh
```

If Keil CLI (`UV4`/`uvision`) is available:

```bash
./scripts/build_firmware.sh --target SmartHome_SingleTask
```

If no Keil CLI but a valid `Project/Objects/SmartHome.hex` already exists:

```bash
./scripts/build_firmware.sh --use-existing
```

## 3. Flash firmware

Auto-select openocd/st-flash:

```bash
./scripts/flash_firmware.sh --method auto --mcu f103
```

Force openocd with board cfg:

```bash
./scripts/flash_firmware.sh --method openocd --mcu f103 --openocd-cfg board/stm32f1discovery.cfg
```

For STM32F407 boards:

```bash
./scripts/flash_firmware.sh --method openocd --mcu f407
```

## 4. Capture UART logs

```bash
./scripts/capture_uart_log.sh /dev/ttyUSB0 115200 ./uart_capture.log
```

## 5. One-key flow

```bash
./scripts/planA_onekey.sh --use-existing --mcu f103 --port /dev/ttyUSB0 --baud 115200
```

Common debug flags:

```bash
./scripts/planA_onekey.sh --use-existing --skip-flash --skip-capture
./scripts/planA_onekey.sh --use-existing --mcu f407 --flash-method openocd
./scripts/planA_onekey.sh --use-existing --mcu f103 --flash-method openocd --openocd-cfg board/stm32f1discovery.cfg
```

## 6. F407 Migration Notes

See:

`F407_MIGRATION_SKELETON.md`
