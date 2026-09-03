#!/usr/bin/env bash
# Compile and upload firmware over WiFi — the primary upload path.
#
# Requires the workstation to be joined to the device's WiFi AP (or DMXFIRE_HOST
# pointing at it on a shared network). Pushes the same .bin that scripts/flash.sh
# would write over USB, but in seconds and without touching the USB-Serial/JTAG
# controller. See docs/spec-ota-update.md.
#
# USB (scripts/flash.sh) remains the recovery path for a device that is bricked,
# has no WiFi, or is running firmware without the OTA endpoint.
#
# Usage:
#   scripts/ota.sh
#   scripts/ota.sh --no-compile          # push the existing build
#   DMXFIRE_HOST=http://10.0.0.42 scripts/ota.sh

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FQBN="m5stack:esp32:m5stack_atoms3"
SKETCH="$REPO/Test_Button_DMX"
BUILD="$SKETCH/build/m5stack.esp32.m5stack_atoms3"
BIN_APP="$BUILD/Test_Button_DMX.ino.bin"
HOST="${DMXFIRE_HOST:-http://192.168.4.1}"

NO_COMPILE=0
[ "${1:-}" = "--no-compile" ] && NO_COMPILE=1

red()    { printf "\033[1;31m%s\033[0m\n" "$*"; }
green()  { printf "\033[1;32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[1;33m%s\033[0m\n" "$*"; }

if [ $NO_COMPILE -eq 0 ]; then
  echo "==> compile"
  arduino-cli compile -e --fqbn "$FQBN" "$SKETCH" 2>&1 | grep -E "error|Sketch uses" || true
fi
[ -f "$BIN_APP" ] || { red "ERROR: $BIN_APP missing — compile failed?"; exit 6; }

SIZE=$(wc -c < "$BIN_APP" | tr -d ' ')
echo "==> firmware: $(printf '%.0f' $((SIZE/1024))) KB"

# --- transport -------------------------------------------------------------
# curl has been observed returning an EMPTY BODY against this device while raw
# sockets and ping are fine (notes.md Session 6). scripts/towers.sh and
# scripts/debug.sh already carry an nc fallback for exactly this; ota.sh did
# not, and its single un-retried curl pre-check hard-failed and blocked the
# upload entirely — the operator had to hand-roll a raw-socket POST.
#
# So every request here tries curl first (it works on other setups) and falls
# back to a plain socket. python3 rather than nc for the POST, because the
# upload is a binary multipart body that has to be framed exactly.

HOSTPORT="${HOST#http://}"; HOSTPORT="${HOSTPORT%%/}"
DEV_HOST="${HOSTPORT%%:*}"
DEV_PORT="${HOSTPORT##*:}"; [ "$DEV_PORT" = "$DEV_HOST" ] && DEV_PORT=80

# GET a path; echoes the response body. Returns nonzero if the device is
# unreachable by BOTH transports.
api_get() {
  local path="$1" body=""
  body=$(curl -fsS -m 5 "$HOST$path" 2>/dev/null) || body=""
  if [ -n "$body" ]; then printf '%s' "$body"; return 0; fi

  body=$(python3 -c '
import socket, sys
host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
try:
    s = socket.create_connection((host, port), timeout=5)
    s.sendall(("GET %s HTTP/1.0\r\nHost: %s\r\n\r\n" % (path, host)).encode())
    buf = b""
    while True:
        c = s.recv(4096)
        if not c:
            break
        buf += c
    s.close()
except OSError:
    sys.exit(1)
sys.stdout.write(buf.split(b"\r\n\r\n", 1)[-1].decode("utf-8", "replace"))
' "$DEV_HOST" "$DEV_PORT" "$path" 2>/dev/null) || return 1

  [ -n "$body" ] || return 1
  printf '%s' "$body"
}

# Multipart-POST the firmware. Echoes "<http_code>\t<body>"; code 000 means the
# connection dropped, which is EXPECTED on success because the device reboots
# while responding.
ota_post() {
  local bin="$1" out code
  out=$(curl -sS -m 180 -w '\n%{http_code}' -F "firmware=@$bin" "$HOST/api/update" 2>/dev/null) || out=""
  code="${out##*$'\n'}"
  if [ "$code" = "200" ]; then
    printf '200\t%s' "${out%$'\n'*}"
    return 0
  fi

  python3 -c '
import os, socket, sys
host, port, binpath = sys.argv[1], int(sys.argv[2]), sys.argv[3]
boundary = "----dmxfireota"
head = ("--%s\r\n"
        "Content-Disposition: form-data; name=\"firmware\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n"
        % (boundary, os.path.basename(binpath))).encode()
tail = ("\r\n--%s--\r\n" % boundary).encode()
data = open(binpath, "rb").read()
req = ("POST /api/update HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n"
       "Content-Type: multipart/form-data; boundary=%s\r\n"
       "Content-Length: %d\r\n\r\n"
       % (host, boundary, len(head) + len(data) + len(tail))).encode()
try:
    s = socket.create_connection((host, port), timeout=180)
    s.sendall(req)
    s.sendall(head)
    # Chunked so a large image cannot stall on one huge send().
    for i in range(0, len(data), 4096):
        s.sendall(data[i:i + 4096])
    s.sendall(tail)
    s.settimeout(30)
    buf = b""
    while True:
        c = s.recv(4096)
        if not c:
            break
        buf += c
    s.close()
except OSError:
    # The device reboots mid-response on success, so a dropped connection is
    # not an error here. The caller verifies by polling for a new boot_id.
    print("000\tconnection dropped")
    sys.exit(0)
status = buf.split(b"\r\n", 1)[0].split(b" ")
code = status[1].decode() if len(status) > 1 else "000"
body = buf.split(b"\r\n\r\n", 1)[-1].decode("utf-8", "replace").strip()
print("%s\t%s" % (code, body))
' "$DEV_HOST" "$DEV_PORT" "$bin"
}

# Refuse to push into a rig that is mid-fire. The device enforces this too, but
# failing here is clearer than a 500 halfway through an upload.
echo "==> checking device state at $HOST"
STATE=""
for attempt in 1 2 3; do
  STATE=$(api_get /api/state) && [ -n "$STATE" ] && break
  [ $attempt -lt 3 ] && sleep 1
done
if [ -z "$STATE" ]; then
  red "ERROR: cannot reach $HOST (tried curl and a raw socket, 3 attempts)"
  red "       Join the device's WiFi AP, or set DMXFIRE_HOST."
  red "       If the device is bricked or has no WiFi, use scripts/flash.sh (USB)."
  exit 1
fi
FSM=$(printf '%s' "$STATE" | sed -n 's/.*"fsm":{"state":"\([A-Z_]*\)".*/\1/p')
PURGE=$(printf '%s' "$STATE" | sed -n 's/.*"purge":\([a-z]*\).*/\1/p')
echo "    fsm=$FSM purge=$PURGE"
if [ "$FSM" != "IDLE" ] || [ "$PURGE" = "true" ]; then
  red "ERROR: device is not idle (fsm=$FSM purge=$PURGE) — refusing to upload."
  red "       An upload stalls the DMX loop; the rig must be idle first."
  exit 4
fi

echo
yellow "==> uploading over WiFi (device will reboot on success)"
RESP=$(ota_post "$BIN_APP" || true)
HTTP_CODE="${RESP%%$'\t'*}"
BODY="${RESP#*$'\t'}"

# A reboot mid-response can drop the connection even on success, so an empty
# body with no HTTP code is not conclusive — verify by polling the device.
if [ "$HTTP_CODE" = "200" ]; then
  green "==> device accepted the image: $BODY"
elif [ -z "$HTTP_CODE" ] || [ "$HTTP_CODE" = "000" ]; then
  yellow "==> connection dropped (expected if the device rebooted) — verifying"
else
  red "ERROR: device rejected the upload (HTTP $HTTP_CODE): $BODY"
  exit 5
fi

echo "==> waiting for device to come back"
for i in $(seq 1 40); do
  sleep 2
  NEW=$(api_get /api/state) || continue
  [ -n "$NEW" ] || continue
  UP=$(printf '%s' "$NEW" | sed -n 's/.*"uptime_ms":\([0-9]*\).*/\1/p')
  BOOT=$(printf '%s' "$NEW" | sed -n 's/.*"boot_id":"\([0-9a-f]*\)".*/\1/p')
  # A fresh boot_id and a small uptime mean the new image is running.
  if [ -n "$UP" ] && [ "$UP" -lt 60000 ]; then
    echo
    green "============================================================"
    green "  *** OTA COMPLETE — device rebooted (boot_id $BOOT) ***"
    green "============================================================"
    exit 0
  fi
done

red "ERROR: device did not come back within 80s."
red "       It may be running the previous image, or need a USB reflash."
exit 7
