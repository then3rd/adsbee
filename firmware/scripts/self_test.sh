#!/usr/bin/env bash
# Self-test the justfile. MODE: expand (default) | software | hardware
#   expand   — parse, dry-run every recipe, check referenced paths (fast, no Docker)
#   software — expand + actually run the no-hardware targets (needs Docker)
#   hardware — software + device targets; auto-verified where observable (bootsel/flash via USB,
#   health via exit code), prompts only for eyeball-only ones (gdb/gui)
#
# Invoked by `just self-test` with FW_DIR/SCRIPTS/CI_DIR/OD_CPP/SET_HH exported as env vars.
set -uo pipefail

mode="${1:-expand}"
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
for f in "$FW_DIR/build.sh" \
         "$SCRIPTS/setup_dev.sh" "$SCRIPTS/check_version_sync.sh" \
         "$SCRIPTS/detect_adsbee_port.sh" "$SCRIPTS/flash.sh" "$SCRIPTS/monitor_auto.sh" \
         "$SCRIPTS/serial_logger/serial_logger.py" "$SCRIPTS/adsbee_monitor/adsbee_monitor.py" \
         "$SCRIPTS/jlink/open_rp2040_core0_jlink.bash" \
         "$SCRIPTS/jlink/open_rp2040_core1_jlink.bash" \
         "$SCRIPTS/jlink/open_cc1312r_jlink.bash" \
         "$SCRIPTS/udev/99-adsbee.rules" \
         "$CI_DIR/reboot_to_bootloader.py" "$CI_DIR/test_ota.py" "$CI_DIR/at_probe.py" \
         "$CI_DIR/check_device.py" "$CI_DIR/requirements.txt" \
         "$OD_CPP" "$SET_HH"; do
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
                # auto/rc recipes are expected to finish on their own (at_probe.py and
                # monitor_auto.sh already have their own internal timeouts) — this is
                # just a backstop so a stuck one can't hang the whole hardware run.
                *)     timeout 60 just "$@"; rc=$? ;;
            esac
            case "$verify" in
                auto:*)
                    if [ "$rc" -eq 0 ] && _wait_usb "${verify#auto:}" 20; then ok=$((ok + 1)); echo "    ✓ $label (exit 0, auto-verified: ${verify#auto:} enumerated)"
                    elif [ "$rc" -ne 0 ]; then fail=1; echo "    ✗ $label (exit $rc)"
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
