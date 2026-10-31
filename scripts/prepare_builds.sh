#!/usr/bin/env bash
# Create build source trees only. This script DOES NOT flash or erase anything.
set -euo pipefail
PACKAGE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${1:-$HOME/halloween-bluetooth-dial-build}"
test -n "${IDF_PATH:-}" || { echo 'Activate ESP-IDF export.sh and retry'; exit 1; }
command -v git >/dev/null || { echo 'git is required'; exit 1; }
test ! -e "$ROOT" || { echo "Refusing to overwrite $ROOT"; exit 1; }
mkdir -p "$ROOT"
# The community display BSP is only a starting point; pinout to be validated on this physical SKU.
git clone --depth 1 https://github.com/jpowen898/lvgl_hello_world.git "$ROOT/s3"
python "$PACKAGE/s3/prepare_s3.py" "$ROOT/s3"
python "$PACKAGE/classic/prepare_classic.py" "$ROOT/classic"
for target in s3 classic; do
  echo "=== $target SOURCE READY === $ROOT/$target"
done
printf '\nESP32-S3 build (after checking actual USB orientation):\n  cd %s/s3 && idf.py set-target esp32s3 && idf.py reconfigure && python %s/scripts/patch_crc_mp3.py %s/s3 && idf.py build\n' "$ROOT" "$PACKAGE" "$ROOT"
printf '\nClassic ESP32 build:\n  cd %s/classic && idf.py set-target esp32 && idf.py build\n' "$ROOT"
echo 'DO NOT FLASH until each chip is separately identified and full-chip backup exists.'