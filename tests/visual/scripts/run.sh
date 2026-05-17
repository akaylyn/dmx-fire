#!/usr/bin/env bash
# Run the full visual test loop:
#   1. (optional) flash firmware:    --flash
#   2. confirm device reachable
#   3. run visual_test.py — creates a new timestamped run directory
#
# Usage:
#   tests/visual/scripts/run.sh             # just run tests
#   tests/visual/scripts/run.sh --flash     # also compile + upload firmware
#   tests/visual/scripts/run.sh --label X   # tag the run directory

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

FLASH=0
LABEL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --flash) FLASH=1; shift;;
    --label) LABEL="$2"; shift 2;;
    *) echo "unknown arg: $1"; exit 64;;
  esac
done

if [ $FLASH -eq 1 ]; then
  echo "==> flashing firmware"
  "$HERE/flash.sh"
  echo "==> waiting 4s for AP to come back up"
  sleep 4
fi

echo "==> network check"
"$HERE/check_network.sh"

VENV="$REPO/.venv/bin/python"
if [ ! -x "$VENV" ]; then
  echo "ERROR: $VENV missing — run: python3 -m venv .venv && .venv/bin/pip install -r tests/requirements.txt pyserial"
  exit 3
fi

ARGS=()
[ -n "$LABEL" ] && ARGS+=("--label" "$LABEL")

echo "==> running visual_test.py"
"$VENV" "$HERE/visual_test.py" "${ARGS[@]}"
