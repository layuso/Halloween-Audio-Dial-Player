#!/usr/bin/env bash
# Read-only device inventory. NEVER select a flash target just from ttyACM0.
set -euo pipefail
hostname
ls -la /dev/serial/by-id/ 2>/dev/null || true
lsusb 2>/dev/null || true
for port in /dev/ttyACM* /dev/ttyUSB*; do
  [[ -c "$port" ]] || continue
  echo "--- $port ---"
  udevadm info -q property -n "$port" | grep -E 'ID_SERIAL=|ID_MODEL=|ID_PATH=' || true
  python -m esptool --port "$port" flash-id || true
done
command -v idf.py || true
ls -lh /home/layuso/Witches_Sound_cleaned_louder.mp3 2>/dev/null || true
