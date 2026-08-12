$ErrorActionPreference = "Stop"

$project = Join-Path $PSScriptRoot "..\Project\SmartHome_F407.uvprojx"
$target = "SmartHome_F407_B"
$axf = Join-Path $PSScriptRoot "..\Project\Objects\SmartHome_F407_B.axf"

$uv4 = Get-Command UV4.exe -ErrorAction SilentlyContinue
if (-not $uv4) { $uv4 = Get-Command UV4 -ErrorAction SilentlyContinue }
if (-not $uv4) { $uv4 = Get-Command uvision.exe -ErrorAction SilentlyContinue }
if (-not $uv4) { $uv4 = Get-Command uvision -ErrorAction SilentlyContinue }
if (-not $uv4) {
  throw "Keil CLI not found (UV4/uvision)."
}

& $uv4.Source -b $project -t $target
if ($LASTEXITCODE -ne 0) {
  exit $LASTEXITCODE
}

if (-not (Test-Path $axf)) {
  throw "Build succeeded but output not found: $axf"
}

Write-Host "[OK] APP_B AXF: $axf"
