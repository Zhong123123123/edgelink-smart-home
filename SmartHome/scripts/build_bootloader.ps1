param(
  [string]$BuildType = "Release",
  [string]$BuildDir = "build-gcc"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$BootDir = Join-Path $Root "bootloader"
$OutDir = Join-Path $BootDir $BuildDir

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
  throw "cmake not found in PATH"
}

if (Get-Command ninja -ErrorAction SilentlyContinue) {
  cmake -S $BootDir -B $OutDir -G "Ninja" -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_TOOLCHAIN_FILE="$BootDir/cmake/arm-none-eabi-gcc.cmake"
} else {
  cmake -S $BootDir -B $OutDir -DCMAKE_BUILD_TYPE=$BuildType -DCMAKE_TOOLCHAIN_FILE="$BootDir/cmake/arm-none-eabi-gcc.cmake"
}
cmake --build $OutDir -- -v

Write-Host "[OK] bootloader build done"
Write-Host "      elf: $OutDir/bootloader.elf"
Write-Host "      hex: $OutDir/bootloader.hex"
Write-Host "      bin: $OutDir/bootloader.bin"
