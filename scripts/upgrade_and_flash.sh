#!/usr/bin/env bash
# Incrementally update existing Halloween firmware and flash ONE identified MCU.
# Does not back up or erase the entire flash chip.
set -euo pipefail
REPO="$(cd -- "$(dirname -- "$0")/.." && pwd)"
ROOT=/home/layuso/halloween-manual/halloween-dial-manual-20261031151306-1
CLASSIC="$ROOT/classic"
S3="$ROOT/s3"
export IDF_PATH=/home/layuso/esp/esp-idf
export IDF_TOOLS_PATH=/home/layuso/.espressif
test -f "$IDF_PATH/export.sh"
test -f "$CLASSIC/build/project_description.json"
test -f "$S3/build/project_description.json"

set +u
source "$IDF_PATH/export.sh" >/dev/null
set -u

FINGERPRINT="$(sha256sum "$REPO/classic/prepare_classic.py" "$REPO/classic/bridge.c" "$REPO/s3/main.cpp" "$REPO/s3/prepare_s3.py" | sha256sum | cut -d' ' -f1)"
STAMP="$ROOT/.halloween-update-fingerprint"
if [[ "$(cat "$STAMP" 2>/dev/null || true)" != "$FINGERPRINT" ]]; then
  TMP="$(mktemp -d)"
  trap 'rm -rf -- "$TMP"' EXIT
  python3 "$REPO/classic/prepare_classic.py" "$TMP/classic"
  cp "$TMP/classic/main/main.c" "$CLASSIC/main/main.c"
  cp "$TMP/classic/main/CMakeLists.txt" "$CLASSIC/main/CMakeLists.txt"
  cp "$TMP/classic/main/bridge.c" "$CLASSIC/main/bridge.c"
  cp "$TMP/classic/main/bridge.h" "$CLASSIC/main/bridge.h"
  cp "$REPO/s3/main.cpp" "$S3/main/main.cpp"
  python3 - "$S3/main/display_init.cpp" "$S3/main/CMakeLists.txt" "$S3/sdkconfig" <<'PY'
from pathlib import Path
import sys
panel, cmake, sdk = (Path(v) for v in sys.argv[1:])
s = panel.read_text()
old = '{0x21, (uint8_t[]){0x00}, 1, 0},'
new = '{0x20, (uint8_t[]){0x00}, 1, 0},'
if old in s:
    assert s.count(old) == 1
    panel.write_text(s.replace(old, new))
elif new not in s:
    raise SystemExit('Unknown panel inversion command: nothing will flash')
s = cmake.read_text()
if 'user_encoder_bsp' not in s:
    needle = 'chmorgan__esp-audio-player'
    assert s.count(needle) == 1
    cmake.write_text(s.replace(needle, 'user_encoder_bsp ' + needle))
s = sdk.read_text()
for size in (20, 24):
    s = s.replace(f'# CONFIG_LV_FONT_MONTSERRAT_{size} is not set',
                  f'CONFIG_LV_FONT_MONTSERRAT_{size}=y')
sdk.write_text(s)
PY
  printf '%s\n' "$FINGERPRINT" > "$STAMP"
fi

echo "Building updated Bluetooth Classic firmware..."
( cd "$CLASSIC" && idf.py reconfigure && idf.py build )
echo "Building updated S3 firmware..."
( cd "$S3" && idf.py reconfigure && idf.py build )
test -s "$CLASSIC/build/a2dp_source.bin"
test -s "$S3/build/lvgl_hello_world.bin"

NUM_PORTS="$(find /dev/serial/by-id -maxdepth 1 -type l 2>/dev/null | wc -l)"
if [[ "$NUM_PORTS" != 1 ]]; then
  echo "STOP: expected one connected serial device; found $NUM_PORTS"
  exit 1
fi
PORT="$(find /dev/serial/by-id -maxdepth 1 -type l | head -1)"
if ! INFO="$(python -m esptool --port "$PORT" flash_id 2>&1)"; then
  printf '%s\n' "$INFO"
  echo "STOP: cannot identify connected chip; nothing flashed"
  exit 1
fi
printf '%s\n' "$INFO" | grep -E 'Chip is |Chip type:|Detected flash size:|MAC:' || true
CHIP="$(printf '%s\n' "$INFO" | sed -nE 's/^Chip (is |type: )([^ (]+).*/\2/p' | head -1)"
MAC="$(printf '%s\n' "$INFO" | grep -Eo '([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}' | tail -1 | tr '[:lower:]' '[:upper:]' || true)"
SIZE="$(printf '%s\n' "$INFO" | sed -nE 's/.*[Ff]lash size: *([0-9]+)[[:space:]]*MB.*/\1/p' | tail -1)"
if [[ "$MAC" == "44:BD:8D:7A:B9:BC" || "$MAC" == "90:70:69:EB:1C:74" ]]; then
  echo "STOP: protected Waymarker device, no flash"
  exit 1
fi
if [[ "$CHIP" == ESP32-S3* && "$SIZE" == 16 && -n "$MAC" ]]; then
  TARGET=s3
elif [[ "$CHIP" == ESP32* && "$CHIP" != ESP32-S3* && "$SIZE" == 4 && "$MAC" == "48:3F:DA:D3:49:60" ]]; then
  TARGET=classic
else
  echo "STOP: unexpected chip / MAC / flash capacity, no flash"
  exit 1
fi
echo "Flashing $TARGET firmware (no full-chip backup)"
( cd "$ROOT/$TARGET" && idf.py -p "$PORT" flash )
echo "FLASH_SUCCESS=$TARGET"
echo "Rotate the DEVICE-side USB-C plug 180 degrees and rerun this same script for the other MCU."
