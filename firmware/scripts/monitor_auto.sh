#!/usr/bin/env bash
# Log for a few seconds and check timestamped lines actually streamed —
# auto-verifiable stand-in for `monitor`.
# Invoked by `just monitor-auto` with PY/PORT/BAUD/SCRIPTS exported as env vars.
set -uo pipefail

log=$(mktemp)
trap 'rm -f "$log"' EXIT
# -o writes plain "[HH:MM:SS.mmm] [LABEL] line" text with no ANSI color codes,
# unlike stdout — much easier to grep reliably.
"$PY" "$SCRIPTS/serial_logger/serial_logger.py" \
    -p "$PORT:$BAUD:RP2040" -o "$log" >/dev/null 2>&1 &
logger_pid=$!
sleep 1
# The device may be idle/silent (no RF traffic, LOG_LEVEL=SILENT) — don't rely
# on ambient output, provoke a guaranteed line by asking a question over a
# second, non-exclusive open of the same port (harmless while the logger reads).
"$PY" -c "import serial; s = serial.Serial('$PORT', $BAUD, timeout=1); s.write(b'AT+HELP\r\n'); s.close()" || true
sleep 2
kill -INT "$logger_pid" 2>/dev/null
wait "$logger_pid" 2>/dev/null
cat "$log"
grep -qE '^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}\] \[' "$log"
