#!/usr/bin/env bash
# Run the test suite. Two modes:
#   --api     run only the pytest API tests (device must be reachable on 192.168.4.1)
#   --visual  run the full visual test loop (requires camera + device on AP)
#   --all     both of the above (default)
#
# Usage:
#   scripts/test.sh
#   scripts/test.sh --api
#   scripts/test.sh --visual
#   scripts/test.sh --visual --flash    # flash firmware first, then visual tests

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$REPO/tests/visual/scripts"

MODE="all"
FLASH=0
while [ $# -gt 0 ]; do
  case "$1" in
    --api)    MODE="api";    shift;;
    --visual) MODE="visual"; shift;;
    --all)    MODE="all";    shift;;
    --flash)  FLASH=1;       shift;;
    *) echo "unknown arg: $1"; exit 64;;
  esac
done

VENV="$REPO/.venv/bin/python"
if [ ! -x "$VENV" ]; then
  echo "==> setting up Python venv"
  python3 -m venv "$REPO/.venv"
  "$REPO/.venv/bin/pip" install --quiet -r "$REPO/tests/requirements.txt" pyserial
fi

if [ "$MODE" = "api" ] || [ "$MODE" = "all" ]; then
  echo "==> network check"
  "$HERE/check_network.sh"
  echo "==> running pytest"
  "$REPO/.venv/bin/pytest" "$REPO/tests" -v
fi

if [ "$MODE" = "visual" ] || [ "$MODE" = "all" ]; then
  ARGS=()
  [ $FLASH -eq 1 ] && ARGS+=("--flash")
  echo "==> running visual tests"
  "$HERE/run.sh" "${ARGS[@]}"
fi
