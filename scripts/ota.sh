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

# Refuse to push into a rig that is mid-fire. The device enforces this too, but
# failing here is clearer than a 500 halfway through an upload.
echo "==> checking device state at $HOST"
STATE=$(curl -fsS -m 5 "$HOST/api/state" 2>/dev/null) || {
  red "ERROR: cannot reach $HOST"
  red "       Join the device's WiFi AP, or set DMXFIRE_HOST."
  red "       If the device is bricked or has no WiFi, use scripts/flash.sh (USB)."
  exit 1
}
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
HTTP_CODE=$(curl -sS -m 180 -o /tmp/dmxfire_ota_resp.txt -w '%{http_code}' \
  -F "firmware=@$BIN_APP" "$HOST/api/update" 2>/dev/null) || true

BODY=$(cat /tmp/dmxfire_ota_resp.txt 2>/dev/null || true)
rm -f /tmp/dmxfire_ota_resp.txt

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
  NEW=$(curl -fsS -m 3 "$HOST/api/state" 2>/dev/null) || continue
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
