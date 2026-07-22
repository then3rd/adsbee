# ADSBee 1090 — one-stop task runner.
# Install just: https://github.com/casey/just   Run `just` (or `just help`) for the menu.
#
# Debug builds: prefix any build/test with `debug=true`, e.g. `debug=true just build`.

set shell := ["bash", "-uc"]

debug := "false"

# ─── Derived from `debug` ─────────────────────────────────────────────────────
_flag := if debug == "true" { "-d" } else { "" }
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
bootsel: _ensure-udev
    {{ PY }} {{ CI_DIR }}/reboot_to_bootloader.py --port {{ PORT }}

# Flash combined.uf2 over USB (BOOTSEL → copy to RPI-RP2). No network needed.
# Pass a path to flash a different image: `just flash path/to/combined.uf2`
flash IMG=UF2: _ensure-udev
    #!/usr/bin/env bash
    set -uo pipefail            # no -e: the RP2040 disconnects mid-copy, which is expected
    img="{{ IMG }}"
    [ -f "$img" ] || { echo "✗ UF2 not found: $img — run 'just build' first."; exit 1; }

    # RPI-RP2 as a block device = board is in BOOTSEL; as a dir = already mounted.
    rp2_dev() { lsblk -rno NAME,LABEL 2>/dev/null | awk '$2=="RPI-RP2"{print "/dev/"$1; exit}'; }
    rp2_mnt() { find /run/media /media /mnt "$HOME" -maxdepth 4 -name RPI-RP2 -type d 2>/dev/null | head -1; }

    # 1. Get the board into BOOTSEL (skip the reboot if it already is).
    if [ -n "$(rp2_dev)$(rp2_mnt)" ]; then
        echo "→ Device already in BOOTSEL."
    else
        echo "→ Rebooting {{ PORT }} into BOOTSEL..."
        {{ PY }} {{ CI_DIR }}/reboot_to_bootloader.py --port {{ PORT }} 2>/dev/null \
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
    #!/usr/bin/env bash
    set -uo pipefail
    py_abs="{{ PY }}"
    [[ "$py_abs" = /* ]] || py_abs="{{ justfile_directory() }}/{{ PY }}"
    log=$(mktemp)
    trap 'rm -f "$log"' EXIT
    # -o writes plain "[HH:MM:SS.mmm] [LABEL] line" text with no ANSI color codes,
    # unlike stdout — much easier to grep reliably.
    "$py_abs" {{ SCRIPTS }}/serial_logger/serial_logger.py \
        -p {{ PORT }}:{{ BAUD }}:RP2040 -o "$log" >/dev/null 2>&1 &
    logger_pid=$!
    sleep 1
    # The device may be idle/silent (no RF traffic, LOG_LEVEL=SILENT) — don't rely
    # on ambient output, provoke a guaranteed line by asking a question over a
    # second, non-exclusive open of the same port (harmless while the logger reads).
    "$py_abs" -c "import serial; s = serial.Serial('{{ PORT }}', {{ BAUD }}, timeout=1); s.write(b'AT+HELP\r\n'); s.close()" || true
    sleep 2
    kill -INT "$logger_pid" 2>/dev/null
    wait "$logger_pid" 2>/dev/null
    cat "$log"
    grep -qE '^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] \[' "$log"

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
    #!/usr/bin/env bash
    set -uo pipefail
    mode="{{ MODE }}"
    case "$mode" in expand|software|hardware) ;; *)
        echo "Unknown MODE '$mode' — use: expand | software | hardware"; exit 2 ;; esac
    fail=0; ran=0; ok=0; skip=0

    # ── expand: parse + dry-run every recipe + path checks (always) ──
    echo "== expand: parse, dry-run, path checks =="
    just --list >/dev/null || { echo "  ✗ parse failed"; exit 1; }
    for r in $(just --summary); do
        [ "$r" = "self-test" ] && continue   # don't recurse into ourselves
        if out=$(just --dry-run "$r" 2>&1); then echo "  ✓ dry-run $r"
        else echo "  ✗ dry-run $r"; echo "$out" | sed 's/^/      /'; fail=1; fi
    done
    for f in "{{ FW_DIR }}/build.sh" \
             "{{ SCRIPTS }}/setup_dev.sh" "{{ SCRIPTS }}/check_version_sync.sh" \
             "{{ SCRIPTS }}/jlink/open_rp2040_core0_jlink.bash" \
             "{{ SCRIPTS }}/jlink/open_rp2040_core1_jlink.bash" \
             "{{ SCRIPTS }}/jlink/open_cc1312r_jlink.bash" \
             "{{ SCRIPTS }}/udev/99-adsbee.rules" \
             "{{ CI_DIR }}/reboot_to_bootloader.py" "{{ CI_DIR }}/test_ota.py" \
             "{{ CI_DIR }}/check_device.py" "{{ CI_DIR }}/requirements.txt" \
             "{{ OD_CPP }}" "{{ SET_HH }}"; do
        if [ -e "$f" ]; then echo "  ✓ path $f"; else echo "  ✗ MISSING: $f"; fail=1; fi
    done
    if [ "$mode" = "expand" ]; then
        [ "$fail" -eq 0 ] && { echo "✓ self-test (expand) passed"; exit 0; } || { echo "✗ self-test (expand) failed"; exit 1; }
    fi

    # run <fatal:0|1> <timeout_s:0=none> <recipe...> — execute a real target, record result.
    # A timeout kill (124) counts as success (blocking servers that started fine).
    run() {
        local fatal="$1" tmo="$2"; shift 2
        ran=$((ran + 1)); echo "→ run: just $*"
        if [ "$tmo" != 0 ]; then timeout "$tmo" just "$@"; else just "$@"; fi
        local rc=$?
        if [ "$rc" -eq 0 ]; then
            ok=$((ok + 1)); echo "  ✓ $*"
        elif [ "$tmo" != 0 ] && [ "$rc" -eq 124 ]; then
            ok=$((ok + 1)); echo "  ✓ $* (started, timed out as expected)"
        else
            echo "  ✗ $* (exit $rc)"; [ "$fatal" = 1 ] && fail=1
        fi
    }

    # ── software: no hardware required (Docker needed for build/test) ──
    if [ "$mode" = software ] || [ "$mode" = hardware ]; then
        echo "== software: run no-hardware targets =="
        run 1 0 version
        run 1 0 check-versions
        run 1 0 test          # host unit tests (Docker)
        run 1 0 build         # full firmware build (Docker, slow)
    fi

    # ── hardware: run device targets, auto-verifying wherever the outcome is observable ──
    if [ "$mode" = hardware ]; then
        if [ ! -t 0 ]; then
            echo "== hardware: needs an interactive terminal — skipping. =="
        else
            echo "== hardware: device tests (auto-verified where possible) =="
            hw_abort=0
            # USB signatures: 2e8a:000a = running app (CDC), 2e8a:0003 = BOOTSEL.
            _usb() { lsusb 2>/dev/null | grep -q "$1"; }
            _wait_usb() {   # <id> <timeout_s> — succeed once <id> is present
                for _ in $(seq 1 "$2"); do _usb "$1" && return 0; sleep 1; done; return 1
            }
            # hwstep "<label>" "<verify>" <recipe...>
            #   <verify> = auto:<id>  → pass if USB <id> appears within 20s (no human)
            #            = rc         → pass if the recipe exits 0
            #            = ask:<q>    → run, then ask the human <q>  (eyeball-only targets)
            hwstep() {
                [ "$hw_abort" = 1 ] && return
                local label="$1" verify="$2"; shift 2
                printf '\n• %s\n    command: just %s\n' "$label" "$*"
                read -r -p "    Run this step? [y/N/q] " a
                case "$a" in
                    q|Q) echo "    aborting remaining hardware steps."; hw_abort=1; return ;;
                    y|Y) ran=$((ran + 1)) ;;
                    *)   skip=$((skip + 1)); echo "    ⤳ skipped"; return ;;
                esac
                local rc=0
                case "$verify" in
                    # Eyeball-only targets block forever — bound them so the run can't hang.
                    ask:*) echo "    (auto-closes after 10s — or quit early: Ctrl-] console, Ctrl-C others)"
                           # `timeout` only signals its direct child (just), which never forwards
                           # it down through the recipe shell to the actual serial tool — miniterm
                           # survives and leaves the tty stuck in raw mode. Run in our own process
                           # group instead so the watchdog can hit every descendant at once, with a
                           # SIGINT first (lets miniterm restore the terminal itself) escalating to
                           # SIGKILL, then always force the tty back to sane afterward.
                           # Bash redirects a backgrounded job's stdin to /dev/null, which breaks
                           # miniterm's termios calls — reopen the real tty device explicitly.
                           tty_dev=$(tty 2>/dev/null) || tty_dev=/dev/tty
                           setsid just "$@" < "$tty_dev" 2> >(grep -v 'terminated on line .* by signal' >&2) &
                           jpid=$!
                           ( sleep 10; kill -INT -- -"$jpid" 2>/dev/null; sleep 2; kill -KILL -- -"$jpid" 2>/dev/null ) &
                           watchdog=$!
                           wait "$jpid" 2>/dev/null; rc=$?
                           kill "$watchdog" 2>/dev/null; wait "$watchdog" 2>/dev/null
                           stty sane 2>/dev/null || true ;;
                    *)     just "$@"; rc=$? ;;
                esac
                case "$verify" in
                    auto:*)
                        if _wait_usb "${verify#auto:}" 20; then ok=$((ok + 1)); echo "    ✓ $label (auto-verified: ${verify#auto:} enumerated)"
                        else fail=1; echo "    ✗ $label (auto-check failed: ${verify#auto:} never appeared)"; fi ;;
                    rc)
                        if [ "$rc" -eq 0 ]; then ok=$((ok + 1)); echo "    ✓ $label (exit 0)"
                        else fail=1; echo "    ✗ $label (exit $rc)"; fi ;;
                    ask:*)
                        read -r -p "    ${verify#ask:} [y/N] " v
                        case "$v" in y|Y) ok=$((ok + 1)); echo "    ✓ $label" ;; *) fail=1; echo "    ✗ $label" ;; esac ;;
                esac
            }
            # Auto-verifiable from USB state / exit code:
            hwstep "Reboot into BOOTSEL"          "auto:2e8a:0003" bootsel
            hwstep "Flash firmware over USB"      "auto:2e8a:000a" flash
            hwstep "Poll device health over WiFi" "rc"            health
            # Eyeball-only (content / debugger attach / GUI) — quit each with Ctrl-C / close:
            hwstep "Serial console (send AT+HELP, check for reply)" "rc"                                 console-auto
            hwstep "Serial logger (check timestamped lines stream)"  "rc"                                 monitor-auto
            hwstep "J-Link GDB server — RP2040 core 0 (Ctrl-C)"      "ask:Connected target reported?"    gdb-rp2040
            hwstep "J-Link GDB server — RP2040 core 1 (Ctrl-C)"      "ask:Connected target reported?"    gdb-rp2040-core1
            hwstep "J-Link GDB server — CC1312 (Ctrl-C)"             "ask:Connected target reported?"    gdb-cc1312
            hwstep "Multi-receiver metrics GUI (close window)"       "ask:GUI opened and showed data?"   receiver-monitor
        fi
    fi

    echo "── executed $ok/$ran ok, $skip skipped ──"
    [ "$fail" -eq 0 ] && { echo "✓ self-test ($mode) passed"; exit 0; } || { echo "✗ self-test ($mode) failed"; exit 1; }

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
