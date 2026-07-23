# ADSBee 1090 — one-stop task runner.
# Install just: https://github.com/casey/just   Run `just` (or `just help`) for the menu.
#
# Debug builds: override `debug` on any build/test, e.g. `just debug=true build`.
# Force a coprocessor reflash (dev cache-buster in the firmware version): `just force=true build`.

set shell := ["bash", "-uc"]

debug := "false"
force := "false"

# ─── Derived from `debug` / `force` ───────────────────────────────────────────
_flag := if debug == "true" { "-d" } else { "" }
_fflag := if force == "true" { "-f" } else { "" }
_btype := if debug == "true" { "Debug" } else { "Release" }

# ─── Paths ────────────────────────────────────────────────────────────────────
FW_DIR := "firmware/adsbee_1090"
SCRIPTS := "firmware/scripts"
CI_DIR := "ci/test_usb_and_ota_flash"
OD_CPP := "firmware/common/coprocessor/object_dictionary.cpp"
SET_HH := "firmware/common/settings/settings.hh"
VENV := ".venv"
# Use the project venv's Python once it exists (created by `just flash-deps`); fall back to system python3 otherwise.
PY := if path_exists(VENV / "bin/python3") == "true" { VENV / "bin/python3" } else { "python3" }

# Build outputs (RP2040 combined image embeds ESP32 + CC1312 binaries).
UF2 := FW_DIR / "pico/build" / _btype / "application/combined.uf2"
OTA := FW_DIR / "pico/build" / _btype / "application/adsbee_1090.ota"

# ─── Device targets — override via env ────────────────────────────────────────
#   ADSBEE_HOST=192.168.1.50 just health      ADSBEE_PORT=/dev/ttyACM1 just console
# PORT auto-detects the RP2040 by USB VID:PID (2e8a:000a) via detect_adsbee_port.sh,
# since its /dev/ttyACM* index shifts depending on what else is plugged in.
HOST := env("ADSBEE_HOST", "adsbee1090.local")
PORT := env("ADSBEE_PORT", shell("bash " + SCRIPTS + "/detect_adsbee_port.sh"))
BAUD := env("ADSBEE_BAUD", "115200")

###@ ADSBee 1090 — build · flash · debug · monitor

default:
    @just help

##@ Setup
# Init git submodules + install version-sync pre-commit hook (run once)
setup: install-udev flash-deps
    bash {{ SCRIPTS }}/setup_dev.sh

# Check the Settings/firmware version-sync rule (working tree vs HEAD)
check-versions:
    {{ SCRIPTS }}/check_version_sync.sh HEAD WORKTREE

# Create/update the project venv and install Python deps used by flash/health/monitor scripts.
# --system-site-packages lets it see system-installed pyserial/tkinter (see SETUP.sh) alongside pip-installed websockets.
flash-deps:
    [ -d {{ VENV }} ] || python3 -m venv --system-site-packages {{ VENV }}
    {{ VENV }}/bin/pip install -r {{ CI_DIR }}/requirements.txt

# Install the udev rule for non-root USB serial access (flash/console), then reload
install-udev:
    sudo cp {{ SCRIPTS }}/udev/99-adsbee.rules /etc/udev/rules.d/99-adsbee.rules
    sudo udevadm control --reload-rules && sudo udevadm trigger
    @echo "✓ udev rule installed. Replug the device (no logout needed)."

# Private: auto-install the udev rule when missing/outdated. Runs before serial
# targets so USB access "just works". Best-effort — never blocks the operation.
_ensure-udev:
    #!/usr/bin/env bash
    src="{{ SCRIPTS }}/udev/99-adsbee.rules"
    dst="/etc/udev/rules.d/99-adsbee.rules"
    # macOS / non-udev systems: nothing to do.
    command -v udevadm >/dev/null 2>&1 || exit 0
    # Already installed and current → silent no-op (no sudo prompt).
    [ -f "$dst" ] && cmp -s "$src" "$dst" && exit 0
    echo "→ udev rule missing/outdated — installing for USB serial access (sudo)…"
    if sudo cp "$src" "$dst" && sudo udevadm control --reload-rules && sudo udevadm trigger; then
        echo "  ✓ installed. If access still fails, replug the device once."
    else
        echo "  ⚠ couldn't auto-install the udev rule — run 'just install-udev' manually." >&2
    fi
    exit 0   # never block the target that depends on us

##@ Build

#@ Set debug=true for Debug builds (e.g. `just debug=true build`)
#@ Set force=true to force a coprocessor reflash (e.g. `just force=true build`)
# Build all targets in order: ESP32 → CC1312 → RP2040 → combined.uf2
build:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} {{ _fflag }} all

# Build ESP32-S3 firmware only
build-esp:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} {{ _fflag }} esp

# Build TI CC1312 firmware only
build-ti:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} {{ _fflag }} ti

# Build RP2040 firmware only (requires esp + ti built first)
build-pico:
    cd {{ FW_DIR }} && bash build.sh {{ _flag }} {{ _fflag }} pico

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
bootsel: _ensure-udev
    {{ PY }} {{ CI_DIR }}/reboot_to_bootloader.py --port {{ PORT }}

# Flash combined.uf2 over USB (BOOTSEL → copy to RPI-RP2). No network needed.
# Pass a path to flash a different image: `just flash path/to/combined.uf2`
flash IMG=UF2: _ensure-udev
    PY={{ PY }} PORT={{ PORT }} CI_DIR={{ CI_DIR }} bash {{ SCRIPTS }}/flash.sh "{{ IMG }}"

# USB flash + WiFi health verify via the CI harness (needs `just flash-deps`)
flash-verify: _ensure-udev
    {{ PY }} {{ CI_DIR }}/test_ota.py --usb-only --uf2 {{ UF2 }} --host {{ HOST }}

# Over-the-air (OTA) firmware upload over WebSocket (device must be on WiFi)
flash-ota:
    {{ PY }} {{ CI_DIR }}/test_ota.py --ota-only --ota-fw {{ OTA }} --host {{ HOST }}

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
console: _ensure-udev
    {{ PY }} -m serial.tools.miniterm {{ PORT }} {{ BAUD }}

# Send AT+HELP and check for a reply — auto-verifiable stand-in for `console`.
console-auto: _ensure-udev
    {{ PY }} "{{ CI_DIR }}/at_probe.py" --port {{ PORT }} --baud {{ BAUD }}

# Timestamped multi-port serial logger (serial_logger; uses poetry if present, else python3 + pyserial)
monitor: _ensure-udev
    #!/usr/bin/env bash
    set -uo pipefail
    # PY may be a repo-root-relative path (.venv/bin/python3) — resolve it to an
    # absolute path before cd'ing into serial_logger, or it silently breaks.
    py_abs="{{ PY }}"
    [[ "$py_abs" = /* ]] || py_abs="{{ justfile_directory() }}/{{ PY }}"
    cd {{ SCRIPTS }}/serial_logger
    if command -v poetry >/dev/null 2>&1; then
        poetry run serial-logger -p {{ PORT }}:{{ BAUD }}:RP2040 -o session.log
    else
        echo "→ poetry not found — running serial_logger.py directly (needs pyserial)." >&2
        "$py_abs" serial_logger.py -p {{ PORT }}:{{ BAUD }}:RP2040 -o session.log
    fi

# Log for a few seconds and check timestamped lines actually streamed — auto-verifiable stand-in for `monitor`.
monitor-auto: _ensure-udev
    PY={{ PY }} PORT={{ PORT }} BAUD={{ BAUD }} SCRIPTS={{ SCRIPTS }} bash {{ SCRIPTS }}/monitor_auto.sh

# Live multi-receiver metrics GUI (needs python websockets + tkinter)
receiver-monitor:
    {{ PY }} {{ SCRIPTS }}/adsbee_monitor/adsbee_monitor.py

# Poll device health over WiFi — ESP32 + RP2040 alive via /metrics
health:
    {{ PY }} {{ CI_DIR }}/check_device.py {{ HOST }}

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

##@ Meta
# Self-test the justfile. MODE: expand (default) | software | hardware
#   expand   — parse, dry-run every recipe, check referenced paths (fast, no Docker)
#   software — expand + actually run the no-hardware targets (needs Docker)
#   hardware — software + device targets; auto-verified where observable (bootsel/flash via USB,
#   health via exit code), prompts only for eyeball-only ones (console/gdb/gui)
#
self-test MODE="expand":
    FW_DIR={{ FW_DIR }} SCRIPTS={{ SCRIPTS }} CI_DIR={{ CI_DIR }} OD_CPP={{ OD_CPP }} SET_HH={{ SET_HH }} \
        bash {{ SCRIPTS }}/self_test.sh {{ MODE }}

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
