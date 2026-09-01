#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "$0")" && pwd)"
uf2="$repo_dir/build-pico-wm8978-blockqueue/pico_wm8978_nam_blockqueue.uf2"

if [[ ! -f "$uf2" ]]; then
  echo "找不到 UF2：$uf2" >&2
  exit 1
fi

echo "等待 Pico 进入 BOOTSEL（RPI-RP2）……按 Ctrl+C 取消。"
# Prefer picotool when the board is already in BOOTSEL.  This avoids the
# transient macOS FAT-volume permission race that can make `cp` fail even
# though the RP2350 mass-storage volume has just appeared.
picotool=""
for candidate in \
  "$(command -v picotool 2>/dev/null || true)" \
  "/tmp/picotool-usb.0ebJ/picotool"; do
  if [[ -n "$candidate" && -x "$candidate" ]]; then
    picotool="$candidate"
    break
  fi
done
if [[ -n "$picotool" ]] && "$picotool" info >/dev/null 2>&1; then
  echo "已通过 picotool 发现 BOOTSEL，直接写入。"
  "$picotool" load -u -v -x "$uf2"
  echo "烧录完成，Pico 应已自动重启。"
  exit 0
fi
volume=""
for _ in {1..120}; do
  while IFS= read -r candidate; do
    [[ -f "$candidate/INFO_UF2.TXT" ]] || continue
    if grep -qiE 'RP2350|RPI-RP2|Raspberry Pi' "$candidate/INFO_UF2.TXT"; then
      volume="$candidate"
      break
    fi
  done < <(find /Volumes -mindepth 1 -maxdepth 1 -type d -print 2>/dev/null)
  [[ -n "$volume" ]] && break
  sleep 1
done

if [[ -z "$volume" ]]; then
  echo "120 秒内未发现 RP2350 BOOTSEL 磁盘。" >&2
  exit 2
fi

echo "已发现：$volume"
grep -E 'MODEL|BOARD|VERSION' "$volume/INFO_UF2.TXT" 2>/dev/null || true
echo "写入：$(basename "$uf2")"
# -X prevents macOS from trying to copy the source file's extended attributes
# to the FAT BOOTSEL volume (which reports Attribute not found).
cp -X "$uf2" "$volume/"
sync
sleep 2
diskutil eject "$volume" >/dev/null || true
echo "烧录完成，Pico 应已自动重启。"
