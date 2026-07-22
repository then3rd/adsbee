#!/usr/bin/env bash
# Find the ADSBee RP2040's CDC serial port by USB VID:PID (2e8a:000a) instead
# of assuming a fixed /dev/ttyACM* index, which shifts whenever another CDC
# device is plugged in first.
set -euo pipefail

VID="2e8a"
PID_APP="000a"
FALLBACK="/dev/ttyACM0"

for dev in /dev/ttyACM* /dev/ttyUSB*; do
    [ -e "$dev" ] || continue
    info=$(udevadm info -q property -n "$dev" 2>/dev/null) || continue
    vid=$(sed -n 's/^ID_VENDOR_ID=//p' <<< "$info")
    pid=$(sed -n 's/^ID_MODEL_ID=//p' <<< "$info")
    if [ "$vid" = "$VID" ] && [ "$pid" = "$PID_APP" ]; then
        echo "$dev"
        exit 0
    fi
done

echo "$FALLBACK"
