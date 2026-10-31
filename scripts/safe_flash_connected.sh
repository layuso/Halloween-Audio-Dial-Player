#!/usr/bin/env bash
# Flash the ONE connected chip only after complete preflash backup and identity verification.
# Deliberately refuses multiple devices, unknown flash size, and known unrelated ESPs.
set -euo pipefail
BUILD_ROOT="$1"
mapfile -t PORTS < <(find /dev/serial/by-id -maxdepth 1 -type l -print 2>/dev/null | sort)
if (( ${#PORTS[@]} != 1 )); then
  echo "FLASH_SKIPPED: expected exactly one USB serial device, found ${#PORTS[@]}"; exit 0
fi
PORT="${PORTS[0]}"
# ESP-IDF 5.5 bundles esptool v4; it uses underscore commands.
# Print the actual probe error instead of hiding it.
if ! INFO="$(python -m esptool --port "$PORT" flash_id 2>&1)"; then
  printf '%s\n' "$INFO"
  echo "FLASH_SKIPPED: ESP flash_id probe failed"
  exit 0
fi
printf '%s\n' "$INFO" | grep -E 'Chip is |Chip type:|flash size:|MAC:' || true
CHIP="$(printf '%s\n' "$INFO" | sed -nE 's/^Chip (is |type: )([^ (]+).*/\2/p' | head -1)"
MAC="$(printf '%s\n' "$INFO" | grep -Eo '([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}' | tail -1 | tr '[:lower:]' '[:upper:]' || true)"
SIZE="$(printf '%s\n' "$INFO" | sed -nE 's/.*[Ff]lash size: *([0-9]+)[[:space:]]*MB.*/\1/p' | tail -1)"
if [[ -z "$CHIP" || -z "$MAC" || -z "$SIZE" ]]; then
  echo "FLASH_SKIPPED: missing chip, MAC, or flash capacity; refusing to guess"
  exit 0
fi
if [[ "$MAC" = "44:BD:8D:7A:B9:BC" || "$MAC" = "90:70:69:EB:1C:74" ]]; then
  echo "FLASH_SKIPPED: this is a protected unrelated device ($MAC)"; exit 0
fi
if [[ "$CHIP" == ESP32-S3* && "$SIZE" == "16" ]]; then
  target=s3; length=0x1000000
elif [[ "$CHIP" == ESP32* && "$CHIP" != ESP32-S3* && "$SIZE" == "4" ]]; then
  target=classic; length=0x400000
else
  echo "FLASH_SKIPPED: unknown chip / flash geometry"; exit 0
fi
test -f "$BUILD_ROOT/$target/build/project_description.json" || { echo "FLASH_SKIPPED: missing verified $target build"; exit 0; }
mkdir -p "$HOME/halloween-dial-factory-backups"
chmod 700 "$HOME/halloween-dial-factory-backups"
BACKUP="$HOME/halloween-dial-factory-backups/$MAC-$target-$GITHUB_RUN_ID.bin"
if test -e "$BACKUP"; then echo "FLASH_SKIPPED: backup path exists"; exit 0; fi
echo "Backing up the full, connected $target flash before changes."
python -m esptool --port "$PORT" read_flash 0 "$length" "$BACKUP"
chmod 600 "$BACKUP"
read_bytes="$(stat -c %s "$BACKUP")"
expected=$((length))
test "$read_bytes" -eq "$expected" || { echo "FLASH_SKIPPED: incomplete backup"; exit 0; }
sha256sum "$BACKUP" | sed -E 's@/.*$@ [locally stored protected backup]@'
echo "Backup verified. Flashing ONLY detected $target MCU."
( cd "$BUILD_ROOT/$target" && idf.py -p "$PORT" flash )
echo "FLASH_SUCCESS=$target"
echo "OTHER_MCU_PENDING=$( [[ $target == s3 ]] && echo classic || echo s3 )"
