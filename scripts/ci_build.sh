#!/usr/bin/env bash
# Build both independent MCU images; flash only when all backups and chip gates pass.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
echo "Host $(hostname), user $(id -un), architecture $(uname -m)"
test "$(uname -m)" = aarch64 || { echo "::error::Expected ARM64 Pi"; exit 1; }
MP3="/home/layuso/Witches_Sound_cleaned_louder.mp3"
test -r "$MP3" || { echo "::error::Cannot read MP3 at $MP3"; exit 1; }
export HALLOWEEN_MP3="$MP3"
file "$MP3"; sha256sum "$MP3"
IDF_SCRIPT="$(find /home /opt -maxdepth 7 -type f -path '*/esp-idf/export.sh' -print -quit 2>/dev/null || true)"
test -f "$IDF_SCRIPT" || { echo "::error::ESP-IDF export.sh not found"; exit 1; }
set +u
source "$IDF_SCRIPT"
set -u
idf.py --version
BUILD_ROOT="$RUNNER_TEMP/halloween-dial-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT"
test ! -e "$BUILD_ROOT" || { echo "::error::Build root exists"; exit 1; }
bash "$ROOT/scripts/prepare_builds.sh" "$BUILD_ROOT"
echo "Building Bluetooth ESP32"
( cd "$BUILD_ROOT/classic" && idf.py set-target esp32 && idf.py build )
echo "Building screen ESP32-S3"
( cd "$BUILD_ROOT/s3" && idf.py set-target esp32s3 && idf.py reconfigure && python "$ROOT/scripts/patch_crc_mp3.py" . && idf.py build )
for t in classic s3; do
  mkdir -p "$ROOT/output/$t"
  cp "$BUILD_ROOT/$t/build/"*.bin "$ROOT/output/$t/"
  ( cd "$ROOT/output/$t" && sha256sum ./*.bin > SHA256SUMS.txt )
done
echo "BUILD_ROOT=$BUILD_ROOT"
echo "HALLOWEEN_BUILD_CLASSIC=passed"
echo "HALLOWEEN_BUILD_S3=passed"
if [[ "$HALLOWEEN_AUTO_FLASH" == "1" ]]; then
  bash "$ROOT/scripts/safe_flash_connected.sh" "$BUILD_ROOT"
fi
