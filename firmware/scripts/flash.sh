#!/usr/bin/env bash
# Flash combined.uf2 over USB (BOOTSEL → copy to RPI-RP2). No network needed.
# Invoked by `just flash` with PY/PORT/CI_DIR exported as env vars; $1 = image path.
set -uo pipefail   # no -e: the RP2040 disconnects mid-copy, which is expected

img="$1"
[ -f "$img" ] || { echo "✗ UF2 not found: $img — run 'just build' first."; exit 1; }

# RPI-RP2 as a block device = board is in BOOTSEL; as a dir = already mounted.
rp2_dev() { lsblk -rno NAME,LABEL 2>/dev/null | awk '$2=="RPI-RP2"{print "/dev/"$1; exit}'; }
rp2_mnt() { find /run/media /media /mnt "$HOME" -maxdepth 4 -name RPI-RP2 -type d 2>/dev/null | head -1; }

# 1. Get the board into BOOTSEL (skip the reboot if it already is).
if [ -n "$(rp2_dev)$(rp2_mnt)" ]; then
    echo "→ Device already in BOOTSEL."
else
    echo "→ Rebooting $PORT into BOOTSEL..."
    "$PY" "$CI_DIR/reboot_to_bootloader.py" --port "$PORT" 2>/dev/null \
        || echo "  (auto-reboot failed — hold BOOTSEL and replug USB)"
fi

# 2. Wait for the RPI-RP2 drive; auto-mount via udisksctl if it isn't already.
echo "→ Waiting for the RPI-RP2 drive..."
mnt=""
for _ in $(seq 1 30); do
    mnt="$(rp2_mnt)"; [ -n "$mnt" ] && break
    dev="$(rp2_dev)"
    if [ -n "$dev" ] && command -v udisksctl >/dev/null 2>&1; then
        udisksctl mount -b "$dev" >/dev/null 2>&1 || true
        mnt="$(rp2_mnt)"; [ -n "$mnt" ] && break
    fi
    sleep 1
done
[ -n "$mnt" ] || { echo "✗ RPI-RP2 not found/mountable. Put the board in BOOTSEL and retry."; exit 1; }

# 3. Copy the image (a write/sync error as the device reboots is normal).
echo "→ Copying $(basename "$img") → $mnt"
cp "$img" "$mnt/" 2>/dev/null; sync 2>/dev/null || true
echo "✓ Flashed — device reboots automatically (reflashes ESP32/CC1312 if versions differ)."
