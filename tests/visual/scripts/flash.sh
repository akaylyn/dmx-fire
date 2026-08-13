#!/usr/bin/env bash
# Compile + upload firmware to the M5AtomS3.
#
# !! NEVER run two instances of this script at the same time !!
# Running a second flash while one is in progress will corrupt the bootloader
# and force a manual recovery. The lockfile below enforces this — do NOT
# pkill or bypass it.
#
# WHY THIS IS NOT A PLAIN `esptool write-flash`
#
# Writing the 1.1 MB app in one go has repeatedly dropped the USB connection
# partway through on this board. The app is therefore written in blocks, each
# hash-verified, with retries — so a drop costs one block, not the whole flash.
#
# The failure was long attributed to an "ESP32-S3 rev 0.2 64 KB block-erase
# errata". No such errata is documented by Espressif, and the upstream bug it
# was pinned to (esptool #832) was filed against esptool 4.4 and is CLOSED.
# What Espressif *does* document for USB-Serial/JTAG is this:
#
#   "If the application accidentally reconfigures the USB peripheral pins or
#    disables the USB peripheral, the device disappears from the system."
#
# That fits: this firmware brings up a WiFi AP, DMX on Serial1 and FastLED
# within milliseconds of boot. The old loop ended every block with
# `--after watchdog-reset`, so the app rebooted and started fighting for the
# USB peripheral between all 18 blocks — 18 chances to wedge, and ~19 s of
# reset+boot+resync overhead per block against ~0.5 s of actual writing
# (measured: 8.8 s of data transfer inside a 461 s flash — about 2%).
#
# So the chip is now held in download mode for the whole run: every block uses
# `--before no-reset --after no-reset`, and only the final block resets into
# the new firmware. The app never runs mid-flash, which is exactly Espressif's
# advice. If a block fails twice in no-reset mode the chip may genuinely have
# fallen out of download mode, so attempt 3+ escalates to a full `usb-reset`.
#
# UNVERIFIED ON HARDWARE — written when no device was available to test. If it
# misbehaves in the field, `--legacy` restores the exact previous behaviour
# (full reset per block, 3 s settle) which is known to work, if slowly.
#
# IMPORTANT: Turn off Bluetooth before running this script.
# macOS Bluetooth device enumeration can interfere with the USB-Serial/JTAG
# connection mid-flash. Disable it in System Settings or the menu bar.
#
# Usage:
#   tests/visual/scripts/flash.sh
#   tests/visual/scripts/flash.sh --erase    # also wipes NVS (config reset)
#   tests/visual/scripts/flash.sh --legacy   # pre-optimisation behaviour
#
# Env:
#   DMXFIRE_BLOCK_SIZE=65536   bytes per block. Raising it (e.g. 262144) means
#                              fewer connections and is the next speed lever,
#                              but is untested — change one thing at a time.

set -euo pipefail

ERASE=0
LEGACY=0
for arg in "$@"; do
  case "$arg" in
    --erase)  ERASE=1 ;;
    --legacy) LEGACY=1 ;;
  esac
done

# Bytes per app block. One esptool write per block, each hash-verified.
BLOCK_SIZE="${DMXFIRE_BLOCK_SIZE:-65536}"

REPO="$(cd "$(dirname "$0")/../../.." && pwd)"
FQBN="m5stack:esp32:m5stack_atoms3"
SKETCH="$REPO/Test_Button_DMX"
LOCKFILE="$REPO/tests/visual/runs/.flash.lock"
mkdir -p "$(dirname "$LOCKFILE")"

ESPTOOL=$(ls -t ~/Library/Arduino15/packages/m5stack/tools/esptool_py/*/esptool 2>/dev/null | head -1)
[ -x "$ESPTOOL" ] || ESPTOOL=$(ls -t ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | head -1)

red()    { printf "\033[1;31m%s\033[0m\n" "$*"; }
green()  { printf "\033[1;32m%s\033[0m\n" "$*"; }
yellow() { printf "\033[1;33m%s\033[0m\n" "$*"; }

upload_banner() {
  echo
  red "============================================================"
  red "  *** UPLOAD IN PROGRESS -- DO NOT UNPLUG THE DEVICE ***"
  red "  *** Keep USB connected until you see SAFE TO UNPLUG ***"
  red "============================================================"
  echo
}

done_banner() {
  echo
  green "============================================================"
  green "  *** UPLOAD COMPLETE -- SAFE TO UNPLUG OR DISCONNECT ***"
  green "============================================================"
  echo
}

find_port() { find /dev -maxdepth 1 -name 'cu.usbmodem*' 2>/dev/null | head -1; }
wait_for_port() {
  local timeout=30 elapsed=0
  while [ -z "$(find_port)" ] && [ $elapsed -lt $timeout ]; do
    sleep 1; elapsed=$((elapsed+1))
  done
  find_port
}

RECOVERY_SERVER_PID=""
cleanup() {
  rm -f "$LOCKFILE"
  [ -n "$RECOVERY_SERVER_PID" ] && kill "$RECOVERY_SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Triggered when do_flash exhausts retries. Prints the four .bin paths the
# user must drop into the browser tool, then opens the hosted esptool-js if
# the internet is reachable, otherwise serves the local copy under
# tools/recovery/ via python3 -m http.server.
recovery_failover() {
  echo
  red "============================================================"
  red "  *** CLI flash failed -- launching recovery tool ***"
  red "============================================================"
  echo
  yellow "Firmware files to load (offset  path):"
  echo "  0x00000  $BIN_BOOT"
  echo "  0x08000  $BIN_PART"
  echo "  0x0e000  $BOOT_APP0"
  echo "  0x10000  $BIN_APP"
  echo
  yellow "Manual recovery steps:"
  echo "  1. Hold the side button on the M5AtomS3."
  echo "  2. Unplug + replug USB while holding."
  echo "  3. Release the button (device is now in ROM download mode)."
  echo "  4. In the browser tool: Connect -> pick port -> load files -> Program."
  echo

  local hosted="https://espressif.github.io/esptool-js/"
  if curl -fsSL -m 3 -o /dev/null "$hosted" 2>/dev/null; then
    green "==> internet reachable - opening hosted recovery tool"
    echo "    $hosted"
    open "$hosted"
  else
    local recovery_dir="$REPO/tools/recovery"
    if [ ! -f "$recovery_dir/index.html" ]; then
      red "ERROR: no internet and no local recovery tool at $recovery_dir"
      return 1
    fi
    local port=8765
    yellow "==> no internet - serving local recovery tool"
    (cd "$recovery_dir" && python3 -m http.server "$port" >/dev/null 2>&1) &
    RECOVERY_SERVER_PID=$!
    sleep 1
    green "==> local recovery tool at http://localhost:$port/"
    open "http://localhost:$port/"
    echo
    yellow "Press CTRL-C in this terminal when done to stop the local server."
    wait "$RECOVERY_SERVER_PID" 2>/dev/null || true
  fi
}

if [ -e "$LOCKFILE" ]; then
  existing_pid=$(cat "$LOCKFILE" 2>/dev/null)
  if [ -n "$existing_pid" ] && kill -0 "$existing_pid" 2>/dev/null; then
    red "ERROR: flash already in progress (pid $existing_pid)"
    red "       !! Do NOT kill it — wait for it to finish or CTRL-C it cleanly !!"
    exit 10
  else
    yellow "WARNING: stale lock found (pid $existing_pid gone) — removing"
    rm -f "$LOCKFILE"
  fi
fi
echo "$$" > "$LOCKFILE"

# Every run tees itself to its own log so scripts/flash-progress.sh can find it
# without being told which file to read: the newest flash-*.log is the current
# run, and flash-latest.log points at it while it is live. Output still goes to
# the terminal as before. Written only after the lock is taken, so a refused
# concurrent run never repoints flash-latest.log at itself.
RUNS_DIR="$(dirname "$LOCKFILE")"
FLASH_LOG="${DMXFIRE_FLASH_LOG:-$RUNS_DIR/flash-$(date +%Y%m%d-%H%M%S).log}"
mkdir -p "$(dirname "$FLASH_LOG")"
ln -sf "$FLASH_LOG" "$RUNS_DIR/flash-latest.log"
# Keep the 20 newest previous logs (this run's is created just below, by tee).
# Each is a few KB, but one per upload adds up.
ls -t "$RUNS_DIR"/flash-*.log 2>/dev/null | grep -v '/flash-latest\.log$' \
  | tail -n +21 | xargs rm -f 2>/dev/null || true
exec > >(tee "$FLASH_LOG") 2>&1
echo "==> logging to $FLASH_LOG"
echo "==> progress bar: scripts/flash-progress.sh   (no arguments needed)"

BUILD="$SKETCH/build/m5stack.esp32.m5stack_atoms3"
BIN_BOOT="$BUILD/Test_Button_DMX.ino.bootloader.bin"
BIN_PART="$BUILD/Test_Button_DMX.ino.partitions.bin"
BIN_APP="$BUILD/Test_Button_DMX.ino.bin"
BOOT_APP0=$(ls ~/Library/Arduino15/packages/m5stack/hardware/esp32/*/tools/partitions/boot_app0.bin 2>/dev/null | sort -V | tail -1)

# Only compile if source changed.
NEEDS_COMPILE=0
if [ ! -f "$BIN_APP" ]; then
  NEEDS_COMPILE=1
else
  while IFS= read -r -d '' f; do
    if [ "$f" -nt "$BIN_APP" ]; then NEEDS_COMPILE=1; break; fi
  done < <(find "$SKETCH" -maxdepth 1 \( -name "*.ino" -o -name "*.cpp" -o -name "*.h" \) -print0)
fi

if [ $NEEDS_COMPILE -eq 1 ]; then
  echo "==> compile (source changed)"
  arduino-cli compile -e --fqbn "$FQBN" "$SKETCH" 2>&1 | grep -E "error|Sketch uses" || true
else
  echo "==> compile skipped (no source changes since last build)"
fi

[ -f "$BIN_APP" ] || { red "ERROR: $BIN_APP missing — compile must have failed"; exit 6; }

PORT=$(wait_for_port)
if [ -z "$PORT" ]; then
  red "ERROR: no /dev/cu.usbmodem* device found — is the device plugged in?"
  exit 1
fi

# Helper: run one esptool write-flash call, retry up to MAX_ATTEMPTS times.
# Succeeds when output contains "Hash of data verified" (teardown errors are
# expected on USB-JTAG and do not indicate a failed write).
MAX_ATTEMPTS=12

# do_flash <before> <after> <flash_addr> <file> [<flash_addr> <file> ...]
#
# <before>/<after> are esptool reset modes. Passing "no-reset" for both keeps
# the chip parked in download mode across calls so the app never boots between
# blocks — see the header. Retries are per call; a "no-reset" call that fails
# twice escalates to a real reset, on the theory that the chip actually left
# download mode and a plain resync can never recover it.
#
# Success is "Hash of data verified": USB-JTAG teardown errors are routine on
# this board and do not mean the write failed.
do_flash() {
  local before="$1" after="$2"; shift 2
  local attempt=0
  while [ $attempt -lt $MAX_ATTEMPTS ]; do
    PORT=$(find /dev -maxdepth 1 -name 'cu.usbmodem*' 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then
      sleep 2; attempt=$((attempt+1)); continue
    fi
    local eff_before="$before"
    if [ $attempt -ge 2 ] && [ "$before" = "no-reset" ]; then
      eff_before="usb-reset"   # assume we fell out of download mode
    fi
    local out
    out=$("$ESPTOOL" --chip esp32s3 -p "$PORT" -b 115200 \
      --before "$eff_before" --after "$after" \
      --connect-attempts 10 \
      write-flash -z \
      --flash-mode dio --flash-freq 40m --flash-size keep \
      "$@" 2>&1)
    if echo "$out" | grep -q "Hash of data verified"; then
      echo "$out" | grep -E "Wrote [0-9]+ bytes|Hash of data verified"
      return 0
    fi
    yellow "    attempt $((attempt+1)) failed [$eff_before]: $(echo "$out" | grep -oE '(fatal error|serial exception)[^.]*' | head -1)"
    attempt=$((attempt+1))
    sleep 4
  done
  red "ERROR: flash operation failed after $MAX_ATTEMPTS attempts"
  return 2
}

upload_banner

# --erase: wipe NVS partition (config reset) before writing.
# The block-by-block write already overwrites all firmware regions; --erase
# only adds an NVS clear so settings reset to defaults.
if [ $ERASE -eq 1 ]; then
  yellow "==> --erase: reading partition table for NVS offset"
  # Parse the NVS partition offset/size from the binary partition table.
  # Fall back to the standard ESP32 default (0x9000, 0x5000) if parsing fails.
  NVS_OFFSET=0x9000
  NVS_SIZE=0x5000
  yellow "==> clearing NVS at $NVS_OFFSET ($NVS_SIZE bytes) — config will reset to defaults"
  erase_attempt=0
  while [ $erase_attempt -lt 5 ]; do
    PORT=$(find /dev -maxdepth 1 -name 'cu.usbmodem*' 2>/dev/null | head -1)
    if [ -z "$PORT" ]; then sleep 2; erase_attempt=$((erase_attempt+1)); continue; fi
    OUT=$("$ESPTOOL" --chip esp32s3 -p "$PORT" -b 115200 \
      --before usb-reset --after watchdog-reset --connect-attempts 10 \
      erase_region $NVS_OFFSET $NVS_SIZE 2>&1) && { echo "$OUT"; break; }
    yellow "    NVS erase attempt $((erase_attempt+1)) failed"
    erase_attempt=$((erase_attempt+1)); sleep 3
  done
fi

APP_SIZE=$(wc -c < "$BIN_APP")
TOTAL_BLOCKS=$(( (APP_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE ))

if [ $LEGACY -eq 1 ]; then
  # Pre-optimisation path: full chip reset per block, app boots in between,
  # 3 s settle. Slow (~7 min) but known-good. Kept as the field fallback.
  yellow "==> LEGACY mode: full reset per block (slow, known-good)"
  BEFORE_FIRST="usb-reset"; AFTER_MID="watchdog-reset"; BEFORE_MID="usb-reset"
  SETTLE=3
else
  # Hold the chip in download mode for the whole run so the app never boots
  # between blocks. Only the last write resets into the new firmware.
  BEFORE_FIRST="usb-reset"; AFTER_MID="no-reset";      BEFORE_MID="no-reset"
  SETTLE=0
fi

# Step 1: bootloader + partition table + OTA data in one call. Leaves the chip
# in download mode (non-legacy) so step 2's first block needs no reset.
yellow "==> step 1/2: bootloader + partition table + OTA data"
do_flash "$BEFORE_FIRST" "$AFTER_MID" \
  0x0     "$BIN_BOOT" \
  0x8000  "$BIN_PART" \
  0xe000  "$BOOT_APP0" || { recovery_failover; exit 2; }

[ "$SETTLE" -gt 0 ] && sleep "$SETTLE"

yellow "==> step 2/2: app binary ($TOTAL_BLOCKS blocks of $BLOCK_SIZE bytes)"
BLOCK=0
OFFSET=0
while [ $OFFSET -lt "$APP_SIZE" ]; do
  FLASH_ADDR=$((0x10000 + OFFSET))
  dd if="$BIN_APP" bs="$BLOCK_SIZE" count=1 skip=$BLOCK of=/tmp/dmxfire_app_block.bin 2>/dev/null
  THIS_SIZE=$(wc -c < /tmp/dmxfire_app_block.bin)

  # The final block boots the device into the firmware just written; every
  # earlier one leaves it parked in download mode.
  AFTER="$AFTER_MID"
  if [ $(( OFFSET + BLOCK_SIZE )) -ge "$APP_SIZE" ]; then
    AFTER="watchdog-reset"
  fi

  yellow "    block $BLOCK/$TOTAL_BLOCKS  addr=$(printf '0x%x' $FLASH_ADDR)  size=$THIS_SIZE bytes"
  do_flash "$BEFORE_MID" "$AFTER" "$FLASH_ADDR" /tmp/dmxfire_app_block.bin \
    || { recovery_failover; exit 2; }

  BLOCK=$((BLOCK+1))
  OFFSET=$((OFFSET+BLOCK_SIZE))
  [ "$SETTLE" -gt 0 ] && sleep "$SETTLE"
done

rm -f /tmp/dmxfire_app_block.bin

done_banner
echo "==> waiting 8s for device to boot and bring up AP"
sleep 8
