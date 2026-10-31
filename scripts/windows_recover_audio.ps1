<#
Recover the embedded Halloween MP3 from this exact ESP32-S3, read-only.
Run in the activated ESP-IDF 5.5 PowerShell. Does NOT flash or erase.
The recovered audio is only accepted when its SHA256 matches the original.
#>
param(
    [string]$Port = "COM3",
    [string]$OutputFile = (Join-Path $HOME "Witches_Sound_cleaned_louder.mp3")
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
# Do not run pyserial's list_ports here: it writes the harmless
# "1 ports found" summary to stderr, which Windows PowerShell 5.1 treats
# as a terminating NativeCommandError under ErrorActionPreference=Stop.
# esptool probes the explicitly requested COM port and verifies chip/MAC.
$probe = @(python -m esptool --port $Port flash_id 2>&1)
if ($LASTEXITCODE -ne 0) { throw "Unable to probe $Port; close any serial monitor and verify connector orientation: $($probe -join ' ')" }
$raw = [string]::Join([Environment]::NewLine,$probe)
Write-Host $raw
$mac = [regex]::Match($raw,'(?im)\bMAC:\s*([0-9a-f:]{17})').Groups[1].Value.ToUpperInvariant()
$chip = [regex]::Match($raw,'(?m)^Chip (?:is |type: )([^\s(]+)').Groups[1].Value
$size = [regex]::Match($raw,'(?im)(?:Detected )?flash size:\s*(\d+)\s*MB').Groups[1].Value
if ($mac -ne "44:BD:8D:7A:B9:BC" -or $chip -notlike "ESP32-S3*" -or $size -ne "16") {
    throw "STOP: only the Halloween S3 MAC 44:BD:8D:7A:B9:BC / 16 MB is authorized. Detected $chip $mac $size MB."
}
$backupDir = Join-Path $repo ".build-windows\backups"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$dump = Join-Path $backupDir "s3-app-$stamp.bin"
Write-Host "READ ONLY: saving 8 MB of S3 app flash to $dump"
python -m esptool --chip esp32s3 --port $Port -b 460800 read_flash 0x10000 0x800000 $dump
if ($LASTEXITCODE -ne 0) { throw "Flash read failed; no firmware was changed." }
if ((Get-Item -LiteralPath $dump).Length -ne 0x800000) { throw "Incomplete read; refusing audio extraction." }
python (Join-Path $repo "scripts\recover_embedded_mp3.py") $dump $OutputFile
if ($LASTEXITCODE -ne 0) { throw "Audio could not be checksum-verified. Preserved read-only dump: $dump" }
Write-Host "RECOVERED_MP3=$OutputFile"
Write-Host "The device firmware was not modified."
