#!/usr/bin/env bash
# Capture ~6 seconds of serial output from the AtomS3.
# Optionally resets the chip first via DTR/RTS to capture the boot banner.
# Output is printed to stdout AND saved to logs/serial/<timestamp>.log in the repo.
#
# Usage:
#   tests/visual/scripts/serial_snapshot.sh           # passive read
#   tests/visual/scripts/serial_snapshot.sh --reset   # reset then read

set -euo pipefail
RESET=0
[ "${1:-}" = "--reset" ] && RESET=1

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
LOCKFILE="$REPO/tests/visual/runs/.flash.lock"

if [ -e "$LOCKFILE" ]; then
  echo "ERROR: a firmware upload is in progress (lock $LOCKFILE)"
  echo "       refusing to open the serial port — wait for the upload to finish"
  exit 9
fi

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -z "$PORT" ]; then
  echo "ERROR: no /dev/cu.usbmodem* device found"
  exit 1
fi

VENV="$REPO/.venv/bin/python"
[ -x "$VENV" ] || VENV=python3

LOGDIR="$REPO/logs/serial"
mkdir -p "$LOGDIR"
TS=$(date -u +%Y%m%dT%H%M%SZ)
LABEL="${2:-}"
LOGFILE="$LOGDIR/${TS}${LABEL:+_$LABEL}.log"

echo "==> serial port: $PORT  (logging to $LOGFILE)"

"$VENV" - "$PORT" "$RESET" "$LOGFILE" <<'PY'
import sys, time, serial
port, reset, logpath = sys.argv[1], sys.argv[2] == "1", sys.argv[3]
s = serial.Serial(port, 115200, timeout=2)
if reset:
    s.setDTR(False); s.setRTS(True);  time.sleep(0.1)
    s.setDTR(True);  s.setRTS(False); time.sleep(0.1)
    s.setDTR(False); s.setRTS(False)
end = time.time() + 6
out = b""
while time.time() < end:
    d = s.read(4096)
    if d:
        out += d
text = out.decode("utf-8", "replace")
print(text)
with open(logpath, "w") as f:
    f.write(text)
s.close()
PY

echo "==> saved: $LOGFILE"
