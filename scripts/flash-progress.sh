#!/usr/bin/env bash
# Render a progress bar for a running (or finished) scripts/flash.sh upload.
#
# flash.sh writes the app one 64 KB block per esptool connection (a dropped USB
# connection then costs one block, not the whole flash — see docs/spec-upload.md),
# so a 1.1 MB firmware is ~18 sequential blocks. That is a long time to stare at
# scrolling esptool output with no sense of how far along it is. This turns the
# log into a progress bar.
#
# It is READ-ONLY: it tails a log file and never touches the device or the
# serial port. Safe to run alongside an in-flight flash — unlike anything that
# opens the port, which would break the upload.
#
# Usage:
#   scripts/flash-progress.sh            # follow the current/most recent flash
#   scripts/flash-progress.sh --once     # print one line and exit
#   scripts/flash-progress.sh <logfile>  # read a specific log
#
# No log file needed: flash.sh writes one per run under tests/visual/runs/ and
# points flash-latest.log at the live one, so this picks the right file itself.
# A path may still be passed for an old run or a hand-captured
# `scripts/flash.sh 2>&1 | tee /tmp/flash.log`.

set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
RUNS_DIR="$REPO/tests/visual/runs"
LOCKFILE="$RUNS_DIR/.flash.lock"

LOG=""
ONCE=""
for arg in "$@"; do
  case "$arg" in
    --once) ONCE="--once" ;;
    -h|--help)
      echo "usage: $(basename "$0") [logfile] [--once]" >&2
      echo "  with no logfile, the current or most recent flash log is used" >&2
      exit 0 ;;
    -*)
      echo "$(basename "$0"): unknown option $arg" >&2
      exit 2 ;;
    *) LOG="$arg" ;;
  esac
done

# Resolve a symlink one hop, handling a relative target.
resolve_link() {
  local link="$1" target
  target="$(readlink "$link" 2>/dev/null)" || return 1
  [[ "$target" != /* ]] && target="$(dirname "$link")/$target"
  printf '%s' "$target"
}

# pid of a flash that is running right now, empty if none.
live_flash_pid() {
  local pid
  [[ -e "$LOCKFILE" ]] || return 1
  pid="$(cat "$LOCKFILE" 2>/dev/null)"
  [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null && printf '%s' "$pid"
}

# Pick the log to read: the live run's log if a flash is in progress, otherwise
# the most recently written one. /tmp is searched too so logs captured the old
# way (tee) are still found.
autodetect_log() {
  local latest="$RUNS_DIR/flash-latest.log" target newest
  if live_flash_pid >/dev/null && [[ -L "$latest" ]]; then
    target="$(resolve_link "$latest")"
    [[ -f "$target" ]] && { printf '%s' "$target"; return 0; }
  fi
  newest="$(ls -t "$RUNS_DIR"/flash-*.log /tmp/flash*.log 2>/dev/null \
    | grep -v '/flash-latest\.log$' | head -1)"
  # DMXFIRE_FLASH_LOG can put the log outside both search paths; the symlink is
  # then the only way to find it.
  if [[ -z "$newest" && -L "$latest" ]]; then
    newest="$(resolve_link "$latest")"
  fi
  [[ -n "$newest" && -f "$newest" ]] && { printf '%s' "$newest"; return 0; }
  return 1
}

if [[ -n "$LOG" ]]; then
  if [[ ! -f "$LOG" ]]; then
    echo "$(basename "$0"): no such log file: $LOG" >&2
    exit 2
  fi
else
  LOG="$(autodetect_log)" || LOG=""
  # A flash started seconds ago holds the lock before it opens its log. Wait
  # that race out, but only when a flash really is running — never hang here
  # just because no flash has ever been logged.
  if [[ -z "$LOG" ]] && live_flash_pid >/dev/null; then
    for _ in $(seq 1 20); do
      sleep 1
      LOG="$(autodetect_log)" || LOG=""
      [[ -n "$LOG" ]] && break
    done
  fi
  if [[ -z "$LOG" ]]; then
    echo "$(basename "$0"): no flash log found — is a flash running?" >&2
    echo "  looked in $RUNS_DIR and /tmp; run scripts/flash.sh (it logs itself)" >&2
    echo "  or pass a log file: $(basename "$0") <logfile>" >&2
    exit 2
  fi
  # Note goes to stderr so stdout stays a single parseable progress line.
  if pid="$(live_flash_pid)"; then
    echo "==> watching $LOG (flash running, pid $pid)" >&2
  else
    echo "==> watching $LOG (no flash running — most recent run)" >&2
  fi
fi

# Total blocks = ceil(app size / 64 KB). Falls back to 18 if the build dir is
# missing, which only affects the denominator, not correctness of the count.
APP_BIN="$REPO/Test_Button_DMX/build/m5stack.esp32.m5stack_atoms3/Test_Button_DMX.ino.bin"
BLOCK_SIZE=65536
if [[ -f "$APP_BIN" ]]; then
  APP_SIZE=$(wc -c < "$APP_BIN" | tr -d ' ')
  TOTAL=$(( (APP_SIZE + BLOCK_SIZE - 1) / BLOCK_SIZE ))
else
  APP_SIZE=0
  TOTAL=18
fi

BAR_WIDTH=32

# strip ANSI colour so grep/sed see plain text
plain() { sed -E $'s/\x1b\\[[0-9;]*[A-Za-z]//g'; }

render() {
  local log="$1" text cur done_ct retries failed safe
  text="$(plain < "$log")"

  # Highest "block N" seen so far; blocks are 0-indexed.
  cur=$(grep -oE 'block[[:space:]]+[0-9]+' <<<"$text" | grep -oE '[0-9]+' | sort -n | tail -1)
  [[ -z "$cur" ]] && cur=-1

  done_ct=$(grep -c 'Hash of data verified' <<<"$text")
  retries=$(grep -c 'attempt [0-9]* failed' <<<"$text")
  # Match flash.sh's actual banners (lines 54 and 82). NOT a bare "SAFE TO
  # UNPLUG" — that substring also appears in the *warning* printed at the START
  # of every flash ("Keep USB connected until you see SAFE TO UNPLUG"), which
  # would report success the moment the upload began.
  failed=$(grep -c 'CLI flash failed' <<<"$text")
  safe=$(grep -c 'UPLOAD COMPLETE' <<<"$text")

  # Count VERIFIED app blocks, not the highest block number started — a block
  # that is mid-retry has been announced but not written, and counting it would
  # report 100% while the last block was still failing.
  #
  # The bootloader/partitions/boot_app0 images verify before the block loop
  # begins, so subtract however many verifies landed before the first "block 0".
  # awk, not `sed -n '1,/re/p'`: BSD sed lacks \+, and an unmatched range prints
  # the whole file, which silently zeroes the count.
  local preamble
  preamble=$(awk '/block[ \t]+[0-9]/{exit} /Hash of data verified/{c++} END{print c+0}' <<<"$text")
  local app_done=$(( done_ct - preamble ))
  (( app_done < 0 )) && app_done=0
  (( app_done > TOTAL )) && app_done=$TOTAL

  # Block currently being attempted (0-indexed -> 1-indexed for display).
  local in_flight=$(( cur + 1 ))
  (( in_flight > TOTAL )) && in_flight=$TOTAL

  local pct=$(( TOTAL > 0 ? app_done * 100 / TOTAL : 0 ))
  local filled=$(( TOTAL > 0 ? app_done * BAR_WIDTH / TOTAL : 0 ))
  local empty=$(( BAR_WIDTH - filled ))

  local bar
  bar="$(printf '%*s' "$filled" '' | tr ' ' '#')$(printf '%*s' "$empty" '' | tr ' ' '.')"

  local status="flashing"
  if (( safe > 0 )); then
    status="DONE — SAFE TO UNPLUG"
  elif (( failed > 0 )); then
    status="FAILED / recovery"
  fi

  printf '[%s] %3d%%  %d/%d verified (on block %d)  retries=%d  %s\n' \
    "$bar" "$pct" "$app_done" "$TOTAL" "$in_flight" "$retries" "$status"
}

if [[ "$ONCE" == "--once" ]]; then
  render "$LOG"
  exit 0
fi

# Follow: redraw whenever the log grows, and stop once the flash concludes.
LAST=""
while true; do
  LINE="$(render "$LOG")"
  if [[ "$LINE" != "$LAST" ]]; then
    printf '\r%s' "$LINE"
    LAST="$LINE"
  fi
  case "$LINE" in
    *"SAFE TO UNPLUG"*|*"FAILED"*) printf '\n'; break ;;
  esac
  sleep 1
done
