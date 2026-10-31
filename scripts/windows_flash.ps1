<#
Chip and MAC-gated Windows flash for Halloween Audio Dial.
No full flash backup, no erase_flash.
Use in IDF 5.5 PowerShell after windows_build.ps1.
#>
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("classic","s3")]
    [string]$Target,
    [string]$Port = "COM6",
    [switch]$RepurposeS3
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF PowerShell is not activated"
}
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$root = Join-Path $repo ".build-windows"
$project = Join-Path $root $Target
$bin = if ($Target -eq 'classic') { 'a2dp_source.bin' } else { 'lvgl_hello_world.bin' }
if (-not (Test-Path (Join-Path $project "build\$bin"))) {
    throw "Missing compiled firmware for $Target. Run .\scripts\windows_build.ps1 -Target $Target first."
}
Write-Host "Identifying serial device on $Port..."
$probe = @(python -m esptool --port $Port flash_id 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Cannot identify device: $($probe -join ' ')" }
$raw = [string]::Join([Environment]::NewLine,$probe)
Write-Host $raw
$chip = [regex]::Match($raw, '(?m)^Chip (?:is |type: )([^\s(]+)').Groups[1].Value
$mac = [regex]::Match($raw, '(?im)\bMAC:\s*([0-9a-f:]{17})').Groups[1].Value.ToUpperInvariant()
$size = [regex]::Match($raw, '(?im)(?:Detected )?flash size:\s*(\d+)\s*MB').Groups[1].Value

if ($Target -eq 'classic') {
    if ($chip -notlike 'ESP32*' -or $chip -like 'ESP32-S3*' -or
        $mac -ne '48:3F:DA:D3:49:60' -or $size -ne '4') {
        throw "STOP: expected Halloween Classic ESP32 / MAC 48:3F:DA:D3:49:60 / 4MB."
    }
} else {
    if ($chip -notlike 'ESP32-S3*' -or $size -ne '16' -or
        $mac -ne '44:BD:8D:7A:B9:BC' -or -not $RepurposeS3) {
        throw "STOP: expected repurposed Halloween S3 / MAC 44:BD:8D:7A:B9:BC / 16MB and -RepurposeS3."
    }
}
Write-Host "VERIFIED $Target on $Port ($mac); flashing without backup..."
Push-Location $project
try {
    idf.py -p $Port flash
    if ($LASTEXITCODE -ne 0) { throw "Flash failed" }
} finally { Pop-Location }
Write-Host "FLASH_SUCCESS=$Target"
