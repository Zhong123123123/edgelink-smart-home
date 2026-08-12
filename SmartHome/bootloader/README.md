# STM32F407 EdgeOTA Bootloader Skeleton

This directory provides a non-RTOS bootloader skeleton for EdgeOTA.

Implemented now:
- flash layout constants for Bootloader/Metadata/APP_A/APP_B/CrashLog.
- metadata struct and CRC32 validation.
- basic slot selection and vector-table jump logic.
- OTA frame type definitions.

Pending hardware validation:
- UART OTA command loop (`OTA_PREPARE/OTA_DATA/OTA_VERIFY/OTA_COMMIT`).
- inactive-slot erase/write and CRC verification against transferred image.
- metadata dual-copy write-back and rollback policy with boot attempts.

Integration note:
- APP project link base should be moved to `0x08020000` for APP_A deployment.
- Bootloader project should own vector table at `0x08000000`.

## Standalone Build (Bootloader Only)

This repo now includes an independent GCC/CMake bootloader project under `bootloader/`.

Prerequisites:
- `arm-none-eabi-gcc` toolchain in PATH
- `cmake` in PATH
- optional: `ninja` in PATH (for faster build)

Build on Windows PowerShell:

```powershell
.\scripts\build_bootloader.ps1
```

Build outputs:
- `bootloader/build-gcc/bootloader.elf`
- `bootloader/build-gcc/bootloader.hex`
- `bootloader/build-gcc/bootloader.bin`

## Keil Standalone Project

If you build with Keil, use the dedicated project:
- `Project/Bootloader_F407.uvprojx`

Key settings already isolated for bootloader:
- target: `Bootloader_F407`
- scatter file: `Project/bootloader_f407.sct` (IROM `0x08000000`, size `64KB`)
- output: `Project/Objects/Bootloader_F407.axf/.hex`

PowerShell CLI build:

```powershell
.\scripts\build_bootloader_keil.ps1
```

## Scripted Hardware Prep

Use scripted checks and packaging before board flashing:

```bash
cd .
./SmartHome/scripts/prepare_ota_release.sh \
  --version 1.1.0 \
  --bin SmartHome/Project/Objects/SmartHome.bin \
  --map SmartHome/Project/Listings/SmartHome.map \
  --hex SmartHome/Project/Objects/SmartHome.hex
```

What the script does:
- verifies `app.bin` size does not exceed slot size (`393216` by default);
- checks map file for expected APP link address (`0x08020000`);
- generates OTA package (`app.bin` + `manifest.json` + optional hex/map) via gateway packager;
- prints next-step flash and gateway registration commands.
