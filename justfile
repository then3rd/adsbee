# ADSBee 1090 — one-stop task runner.
# Install just: https://github.com/casey/just   Run `just` (or `just help`) for the menu.
#
# Debug builds: prefix any build/test with `debug=true`, e.g. `debug=true just build`.

set shell := ["bash", "-uc"]

debug := "false"

# ─── Derived from `debug` ─────────────────────────────────────────────────────
_flag  := if debug == "true" { "-d" } else { "" }
_btype := if debug == "true" { "Debug" } else { "Release" }

# ─── Paths ────────────────────────────────────────────────────────────────────
FW_DIR  := "firmware/adsbee_1090"
SCRIPTS := "firmware/scripts"
CI_DIR  := "ci/test_usb_and_ota_flash"
OD_CPP  := "firmware/common/coprocessor/object_dictionary.cpp"
SET_HH  := "firmware/common/settings/settings.hh"

# Build outputs (RP2040 combined image embeds ESP32 + CC1312 binaries).
UF2 := FW_DIR / "pico/build" / _btype / "application/combined.uf2"
OTA := FW_DIR / "pico/build" / _btype / "application/adsbee_1090.ota"

# ─── Device targets — override via env ────────────────────────────────────────
#   ADSBEE_HOST=192.168.1.50 just health      ADSBEE_PORT=/dev/ttyACM1 just console
HOST := env("ADSBEE_HOST", "adsbee1090.local")
PORT := env("ADSBEE_PORT", "/dev/ttyACM0")
BAUD := env("ADSBEE_BAUD", "115200")

###@ ADSBee 1090 — build · flash · debug · monitor

default:
    @just help

##@ Setup
# Init git submodules + install version-sync pre-commit hook (run once)
setup:
    bash {{ SCRIPTS }}/setup_dev.sh

# Check the Settings/firmware version-sync rule (working tree vs HEAD)
check-versions:
    {{ SCRIPTS }}/check_version_sync.sh HEAD WORKTREE

# Install Python deps used by flash/health scripts (websockets)
flash-deps:
    python3 -m pip install -r {{ CI_DIR }}/requirements.txt

##@ Build

#@ Set debug=true for Debug builds (e.g. `debug=true just build`)
# Build all targets in order: ESP32 → CC1312 → RP2040 → combined.uf2
build:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} all

# Build ESP32-S3 firmware only
build-esp:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} esp

# Build TI CC1312 firmware only
build-ti:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} ti

# Build RP2040 firmware only (requires esp + ti built first)
build-pico:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} pico

# Remove all build directories
clean:
    cd {{ FW_DIR }} && bash build.sh clean

# Clean then build everything from scratch
rebuild: clean build

##@ Test
# Build & run host unit tests; optional ctest -R regex (e.g. `just test AircraftJSON`)
test FILTER="":
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} test {{ FILTER }}

##@ Flash / Install
# Reboot the RP2040 into BOOTSEL (RPI-RP2) mode over USB serial
bootsel:
    python3 {{ CI_DIR }}/reboot_to_bootloader.py --port {{ PORT }}

# Flash combined.uf2 over USB (BOOTSEL → copy to RPI-RP2). No network needed.
# Pass a path to flash a different image: `just flash path/to/combined.uf2`
flash IMG=UF2:
    #!/usr/bin/env bash
    set -euo pipefail
    img="{{ IMG }}"
    [ -f "$img" ] || { echo "UF2 not found: $img — run 'just build' first."; exit 1; }
    echo "→ Rebooting {{ PORT }} into BOOTSEL..."
    python3 {{ CI_DIR }}/reboot_to_bootloader.py --port {{ PORT }} 2>/dev/null \
        || echo "  (auto-reboot failed — hold BOOTSEL and replug USB)"
    echo "→ Waiting for RPI-RP2 mass-storage mount..."
    mnt=""
    for _ in $(seq 1 30); do
        mnt=$(find /run/media /media /mnt "$HOME" -maxdepth 4 -name RPI-RP2 -type d 2>/dev/null | head -1)
        [ -n "$mnt" ] && break
        sleep 1
    done
    [ -n "$mnt" ] || { echo "RPI-RP2 not found. Mount it, then retry."; exit 1; }
    echo "→ Copying $(basename "$img") → $mnt"
    cp "$img" "$mnt/" && sync
    echo "✓ Done — device reboots automatically (reflashes ESP32/CC1312 if versions differ)."

# USB flash + WiFi health verify via the CI harness (needs `just flash-deps`)
flash-verify:
    python3 {{ CI_DIR }}/test_ota.py --usb-only --uf2 {{ UF2 }} --host {{ HOST }}

# Over-the-air (OTA) firmware upload over WebSocket (device must be on WiFi)
flash-ota:
    python3 {{ CI_DIR }}/test_ota.py --ota-only --ota-fw {{ OTA }} --host {{ HOST }}

##@ Debug
#@ SEGGER J-Link GDB servers — attach your debugger to the printed port
# RP2040 core 0 GDB server (port 2331)
gdb-rp2040:
    bash {{ SCRIPTS }}/jlink/open_rp2040_core0_jlink.bash

# RP2040 core 1 GDB server (port 2431)
gdb-rp2040-core1:
    bash {{ SCRIPTS }}/jlink/open_rp2040_core1_jlink.bash

# CC1312 GDB server (port 2337)
gdb-cc1312:
    bash {{ SCRIPTS }}/jlink/open_cc1312r_jlink.bash

##@ Monitor / Observe
# Interactive serial console to the USB AT interface (Ctrl-] to exit)
console:
    python3 -m serial.tools.miniterm {{ PORT }} {{ BAUD }}

# Timestamped multi-port serial logger (serial_logger; needs poetry)
monitor:
    cd {{ SCRIPTS }}/serial_logger && poetry run serial-logger -p {{ PORT }}:{{ BAUD }}:RP2040 -o session.log

# Live multi-receiver metrics GUI (needs python websockets + tkinter)
receiver-monitor:
    python3 {{ SCRIPTS }}/adsbee_monitor/adsbee_monitor.py

# Poll device health over WiFi — ESP32 + RP2040 alive via /metrics
health:
    python3 {{ CI_DIR }}/check_device.py {{ HOST }}

##@ Info
# Print the firmware version from object_dictionary.cpp
version:
    #!/usr/bin/env bash
    set -euo pipefail
    g() { grep -oP "$1 = \K[0-9]+" "{{ OD_CPP }}"; }
    maj=$(g kFirmwareVersionMajor); min=$(g kFirmwareVersionMinor)
    pat=$(g kFirmwareVersionPatch); rc=$(g kFirmwareVersionReleaseCandidate)
    ver="$maj.$min.$pat"; [ "$rc" != "0" ] && ver="$ver-rc$rc"
    echo "firmware  $ver"
    echo "settings  $(grep -oP 'kSettingsVersion = \K[0-9]+' "{{ SET_HH }}")"

##@ Help
# Print this help message.
# Markers: ###@ title  ##@ section  #@ sub-section  # command description
help:
    #!/usr/bin/env bash
    awk '
    /^###@ / { gsub(/^###@ /, ""); printf "\n\033[1;4m%s\033[0m\n", $0; n=0; next }
    /^##@ /  { gsub(/^##@ /, "");  printf "\n\033[1m%s\033[0m\n", $0; n=0; next }
    /^#@ /   { gsub(/^#@ /, "");   printf "\n  \033[2m%s\033[0m\n", $0; n=0; next }
    /^# /    { n++; c[n] = substr($0, 3); next }
    /^[a-z][-a-z0-9_]*/ && !/^set / && !/^help[: ]/ && !/^default[: ]/ {
        if (n > 0) {
            r = $1; sub(/:.*/, "", r);
            printf "  \033[36m%-20s\033[0m %s\n", r, c[1];
            for (i = 2; i <= n; i++) printf "  %-22s%s\n", "", c[i];
        }
        n = 0; next
    }
    { n = 0 }
    ' {{ justfile() }}
