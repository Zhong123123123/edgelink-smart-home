param(
  [string]$Target = "Bootloader_F407",
  [ValidateSet("DEV","PROD")]
  [string]$Mode = "DEV"
)

$ErrorActionPreference = "Stop"
$Proj = Join-Path $PSScriptRoot "..\Project\Bootloader_F407.uvprojx"
$Hex = Join-Path $PSScriptRoot "..\Project\Objects\Bootloader_F407.hex"
$Cfg = Join-Path $PSScriptRoot "..\bootloader\bootloader_config.h"

$uv4 = Get-Command UV4 -ErrorAction SilentlyContinue
if (-not $uv4) {
  $uv4 = Get-Command uvision -ErrorAction SilentlyContinue
}
if (-not $uv4) {
  throw "Keil CLI not found (UV4/uvision)."
}

if ($Mode -eq "PROD") {
  if (-not (Test-Path $Cfg)) {
    throw "Config file not found: $Cfg"
  }
  $cfgText = Get-Content -Path $Cfg -Raw
  if ($cfgText -match "#define\s+BL_DEV_BYPASS_CRC\s+1U") {
    throw "PROD build blocked: BL_DEV_BYPASS_CRC is 1U in bootloader_config.h. Set it to 0U first."
  }
}

& $uv4.Source -b $Proj -t $Target
if (-not (Test-Path $Hex)) {
  throw "Build finished but HEX not found: $Hex"
}

if ($Mode -eq "PROD") {
  Write-Host "[REMINDER] PROD build selected. Ensure BL_DEV_BYPASS_CRC=0 in bootloader/bootloader_config.h"
} else {
  Write-Host "[REMINDER] DEV build selected. BL_DEV_BYPASS_CRC should be 1 for debug convenience."
}
Write-Host "[OK] bootloader hex: $Hex"
