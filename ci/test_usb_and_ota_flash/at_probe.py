#!/usr/bin/env python3
"""
Send an AT command over USB serial and check that the device replies.

Replaces the old "eyeball it in miniterm" hardware-test step with something
auto-verifiable: send AT+HELP, wait for any bytes back, exit 0/1 accordingly.
"""

import argparse
import sys
import time

import serial

CMD = "AT+HELP\r\n"


def probe(port: str, baud: int, timeout_s: float) -> bool:
    print(f"  Sending {CMD.strip()} via {port} ...")
    with serial.Serial(port, baud, timeout=0.2) as ser:
        ser.reset_input_buffer()
        ser.write(CMD.encode())
        deadline = time.monotonic() + timeout_s
        response = b""
        while time.monotonic() < deadline:
            chunk = ser.read(4096)
            if chunk:
                response += chunk
        if response:
            text = response.decode("utf-8", errors="replace").strip()
            print("  Device replied:")
            for line in text.splitlines():
                print(f"    {line}")
            return True
        print("  No response received.")
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=3.0, help="seconds to wait for a reply")
    args = parser.parse_args()

    try:
        ok = probe(args.port, args.baud, args.timeout)
    except FileNotFoundError:
        print(f"  Serial port {args.port} not found. Is the device connected?")
        return 1
    except serial.SerialException as e:
        print(f"  Serial error: {e}")
        return 1

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
