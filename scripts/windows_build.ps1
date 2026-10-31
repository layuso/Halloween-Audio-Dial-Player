<#
Windows native incremental firmware build for Halloween Audio Dial.
Run in an activated Espressif IDF PowerShell terminal (5.5.x).
Does not flash, erase, or read any USB device.
#>
param(
    [ValidateSet("classic","s3","both")]
    [string]$Target = "classic",
    [string]$AudioFile = ""
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw "ESP-IDF is not active. Launch the IDF 5.5.x PowerShell shortcut."
}
$idfVersion = (idf.py --version | Out-String)
Write-Host "Toolchain: $idfVersion"
if ($idfVersion -notmatch '5\.5\.') {
    throw "Expected ESP-IDF 5.5.x, not $idfVersion"
}
$repo = Split-Path -Parent (Split-Path -Parent $PSCommandPath)
$work = Join-Path $repo ".build-windows"
New-Item -ItemType Directory -Force -Path $work | Out-Null
function Invoke-Required([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "Command failed (exit $LASTEXITCODE)" }
}
function Ensure-Clone([string]$path,[string]$url) {
    if (-not (Test-Path $path)) {
        Invoke-Required { git clone $url $path }
    }
}
if ($Target -in @("classic","both")) {
    $classic = Join-Path $work "classic"
    if (-not (Test-Path (Join-Path $classic 'main\main.c'))) {
        Invoke-Required { python (Join-Path $repo "classic\prepare_classic.py") $classic }
    }
    Push-Location $classic
    try {
        if (-not (Test-Path "sdkconfig")) {
            Invoke-Required { idf.py set-target esp32 }
        }
        # If an already-built project was created with the old broken defaults,
        # refuse to silently use it; update its sdkconfig through menuconfig.
        if ((Test-Path "sdkconfig") -and
            -not (Select-String -Path "sdkconfig" -Pattern '^CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y$' -Quiet)) {
            throw "Classic controller is not BR/EDR-only in sdkconfig. Fix with idf.py menuconfig before building."
        }
        Invoke-Required { idf.py build }
    } finally { Pop-Location }
}
if ($Target -in @("s3","both")) {
    if (-not $AudioFile) {
        $AudioFile = Join-Path $HOME "Witches_Sound_cleaned_louder.mp3"
    }
    if (-not (Test-Path -LiteralPath $AudioFile)) {
        throw "S3 build needs the original MP3. Supply -AudioFile 'C:\path\Witches_Sound_cleaned_louder.mp3'."
    }
    $env:HALLOWEEN_MP3 = (Resolve-Path -LiteralPath $AudioFile).Path
    $s3 = Join-Path $work "s3"
    Ensure-Clone $s3 "https://github.com/jpowen898/lvgl_hello_world.git"
    if (-not (Test-Path (Join-Path $s3 ".halloween-injected"))) {
        Invoke-Required { python (Join-Path $repo "s3\prepare_s3.py") $s3 }
    }
    # Keep existing Windows S3 builds in sync with current firmware.
    Copy-Item (Join-Path $repo "s3\main.cpp") (Join-Path $s3 "main\main.cpp") -Force
    $panelPath = Join-Path $s3 "main\display_init.cpp"
    $panelSrc = Get-Content $panelPath -Raw
    $correct = '{0x21, (uint8_t[]){0x00}, 1, 0},'
    $wrong = '{0x20, (uint8_t[]){0x00}, 1, 0},'
    if ($panelSrc.Contains($wrong)) {
        $panelSrc = $panelSrc.Replace($wrong, $correct)
        [System.IO.File]::WriteAllText($panelPath, $panelSrc)
    } elseif (-not $panelSrc.Contains($correct)) {
        throw "Unknown SH8601 panel inversion command. Refusing S3 build."
    }
    Push-Location $s3
    try {
        if (-not (Test-Path "sdkconfig")) {
            Invoke-Required { idf.py set-target esp32s3 }
        }
        # Component Manager must download esp-audio-player before its source
        # can be patched to recognize CRC-protected FF FA MP3 frames.
        Invoke-Required { idf.py reconfigure }
        Invoke-Required { python (Join-Path $repo "scripts\patch_crc_mp3.py") . }
        Invoke-Required { idf.py build }
    } finally { Pop-Location }
}
Write-Host "BUILD_SUCCESS=$Target; outputs in $work"
