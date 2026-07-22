#!/usr/bin/env python3
"""
Reboot ADSBee into RP2040 USB bootloader mode.

Two transport options:
  --host   Send AT+BOOT_USB_UF2=1DEADBEE via the console WebSocket (requires WiFi).
  --port   Send AT+BOOT_USB_UF2=1DEADBEE via USB serial (no WiFi needed).

After the command the RP2040 calls rom_reset_usb_boot() and re-enumerates as
the RPI-RP2 USB mass-storage drive, ready to receive a combined.uf2.
"""

import asyncio
import sys
import argparse
import time
import websockets
import websockets.exceptions

CMD = "AT+BOOT_USB_UF2=1DEADBEE\r\n"


async def reboot_via_websocket(host: str) -> None:
    url = f"ws://{host}/console"
    print(f"  Connecting to {url} ...")
    async with websockets.connect(url, open_timeout=5) as ws:
        print(f"  Sending {CMD.strip()} ...")
        await ws.send(CMD)
        try:
            msg = await asyncio.wait_for(ws.recv(), timeout=2.0)
            text = msg if isinstance(msg, str) else msg.decode("utf-8", errors="replace")
            print(f"  Device: {text.strip()}")
        except asyncio.TimeoutError:
            pass  # device rebooted before responding — that's fine


def reboot_via_serial(port: str) -> None:
    print(f"  Sending {CMD.strip()} via {port} ...")
    # USB CDC ignores baud rate; device reboots immediately so no response expected.
    try:
        with open(port, "wb", buffering=0) as f:
            f.write(CMD.encode())
            time.sleep(0.1)
    except FileNotFoundError:
        raise RuntimeError(f"Serial port {port} not found. Is the device connected?")
    except PermissionError:
        # Report the port's actual owning group; if it's root-owned (common on
        # Arch, where no group grants access) point at the udev rule instead.
        try:
            import grp, os
            group = grp.getgrgid(os.stat(port).st_gid).gr_name
        except Exception:
            group = None
        if group and group != "root":
            hint = (f"add your user to the '{group}' group and re-login: "
                    f"sudo usermod -aG {group} $USER")
        else:
            hint = ("install the udev rule for non-root access: `just install-udev` "
                    "(firmware/scripts/udev/99-adsbee.rules), then replug the device")
        raise RuntimeError(f"Permission denied on {port}. Fix: {hint}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--host", help="Device hostname or IP for WebSocket transport")
    group.add_argument("--port", help="USB serial device for serial transport (e.g. /dev/ttyACM0)")
    args = parser.parse_args()

    if args.port:
        reboot_via_serial(args.port)
    else:
        asyncio.run(reboot_via_websocket(args.host))


if __name__ == "__main__":
    main()
