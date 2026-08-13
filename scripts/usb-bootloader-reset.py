#!/usr/bin/env python3
"""Drive the ESP32-S3 into ROM download mode over USB, no button press.

The M5AtomS3's side button pulls GPIO0 low so the ROM enters download mode. The
ESP32-S3's USB-Serial/JTAG peripheral implements the same classic auto-reset
circuit in hardware, driven by the CDC control lines:

    DTR -> GPIO0   (low = "boot from ROM downloader")
    RTS -> EN      (low = chip reset)

So the button-hold sequence has a software equivalent: assert GPIO0 low, pulse
reset, release reset while GPIO0 is still low, then release GPIO0.

Used when the CLI flash wedges and nobody can reach the device physically.
See docs/spec-flash-recovery-failover.md and CLAUDE.md.

Usage:
    python3 scripts/usb-bootloader-reset.py [port] [--probe]

--probe also reads the port afterwards and prints whatever the ROM emits, which
is how you tell download mode ("waiting for download") from a hung chip.
"""

import glob
import sys
import time

import serial


def find_port() -> str:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port found — is the device connected?")
    return ports[0]


def sequence(ser: serial.Serial, label: str, steps) -> None:
    print(f"  [{label}]", end="", flush=True)
    for dtr, rts, hold in steps:
        ser.dtr = dtr
        ser.rts = rts
        print(f" dtr={int(dtr)},rts={int(rts)}", end="", flush=True)
        time.sleep(hold)
    print()


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    probe = "--probe" in sys.argv
    port = args[0] if args else find_port()
    print(f"port: {port}")

    # Several variants, because which one lands depends on how wedged the
    # USB-Serial/JTAG peripheral is. Each ends with GPIO0 released.
    variants = {
        # Classic esptool "default reset": GPIO0 low, pulse EN, release.
        "classic": [
            (False, False, 0.1),
            (True, False, 0.1),   # GPIO0 low
            (True, True, 0.1),    # EN low  -> in reset, GPIO0 still low
            (True, False, 0.05),  # EN high -> boots, samples GPIO0 low
            (False, False, 0.5),  # release GPIO0
        ],
        # Longer reset hold — helps when the chip is not sampling promptly.
        "slow": [
            (False, False, 0.2),
            (True, False, 0.3),
            (True, True, 0.5),
            (True, False, 0.2),
            (False, False, 0.8),
        ],
        # Both lines asserted together, then staggered release.
        "together": [
            (True, True, 0.4),
            (True, False, 0.3),
            (False, False, 0.5),
        ],
    }

    for name, steps in variants.items():
        try:
            ser = serial.Serial(port, 115200, timeout=1)
        except serial.SerialException as e:
            print(f"  cannot open port: {e}")
            time.sleep(1)
            continue
        try:
            sequence(ser, name, steps)
            ser.reset_input_buffer()
            time.sleep(0.4)
            waiting = ser.in_waiting
            data = ser.read(waiting or 512)
            if data:
                text = data.decode("utf-8", "replace").strip()
                print(f"    -> {len(data)} bytes: {text[:300]!r}")
                if "waiting for download" in text.lower() or "DOWNLOAD" in text:
                    print("    *** DEVICE IS IN DOWNLOAD MODE ***")
                    return
            else:
                print("    -> no data")
        finally:
            ser.close()
        time.sleep(0.5)

    # The 1200-baud touch: a separate, older bootloader-entry convention. Cheap
    # to try and harmless if the chip ignores it.
    print("  [1200-baud touch]")
    try:
        ser = serial.Serial(port, 1200)
        ser.dtr = False
        time.sleep(0.3)
        ser.close()
        time.sleep(1.0)
        print("    done")
    except serial.SerialException as e:
        print(f"    failed: {e}")

    if probe:
        print("\nreading port for 5s...")
        try:
            ser = serial.Serial(port, 115200, timeout=1)
            end = time.time() + 5
            got = b""
            while time.time() < end:
                got += ser.read(256)
            ser.close()
            print(f"  {len(got)} bytes: {got.decode('utf-8', 'replace')[:600]!r}"
                  if got else "  silent — chip is not talking")
        except serial.SerialException as e:
            print(f"  read failed: {e}")


if __name__ == "__main__":
    main()
