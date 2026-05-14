#!/usr/bin/env bash
# Live serial monitor — prints device output in real time and saves to
# logs/serial/<timestamp>.log so every debug session is captured in the repo.
# Reconnects automatically if the device reboots or a new firmware is flashed.
# Press Ctrl-C to stop.
#
# Usage:
#   scripts/monitor.sh

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"

VENV="$REPO/.venv/bin/python"
[ -x "$VENV" ] || VENV=python3

LOGDIR="$REPO/logs/serial"
mkdir -p "$LOGDIR"
TS=$(date -u +%Y%m%dT%H%M%SZ)
LOGFILE="$LOGDIR/${TS}_monitor.log"

echo "==> logging to $LOGFILE  (Ctrl-C to stop)"
echo ""

"$VENV" - "$LOGFILE" <<'PY'
import sys, signal, time, serial, serial.tools.list_ports

logpath = sys.argv[1]

def handle_sigint(sig, frame):
    print("\n==> monitor stopped")
    sys.exit(0)
signal.signal(signal.SIGINT, handle_sigint)

def find_port():
    while True:
        ports = [p.device for p in serial.tools.list_ports.comports()
                 if "usbmodem" in p.device or "usbserial" in p.device]
        if ports:
            return ports[0]
        time.sleep(0.5)

with open(logpath, "w", buffering=1) as f:
    while True:
        port = find_port()
        print(f"==> connected to {port}")
        try:
            s = serial.Serial(port, 115200, timeout=0.1)
            while True:
                data = s.read(4096)
                if data:
                    text = data.decode("utf-8", "replace")
                    sys.stdout.write(text)
                    sys.stdout.flush()
                    f.write(text)
        except serial.SerialException:
            print("==> port lost, waiting for device to reconnect...")
            time.sleep(1)
PY
