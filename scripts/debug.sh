#!/usr/bin/env bash
# Debug snapshot: check network, capture serial, print API state.
# Saves serial log to logs/serial/ in the repo.
#
# Usage:
#   scripts/debug.sh             # serial read (passive)
#   scripts/debug.sh --reset     # reset device first, then read boot banner

set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"

echo "==> network check"
"$REPO/tests/visual/scripts/check_network.sh" || true

echo ""
echo "==> serial snapshot"
"$REPO/tests/visual/scripts/serial_snapshot.sh" "$@"

echo ""
echo "==> API state"
HOST="${DMXFIRE_HOST:-192.168.4.1}"
(printf "GET /api/state HTTP/1.0\r\nHost: %s\r\n\r\n" "$HOST"; sleep 1) \
  | nc -w 4 "$HOST" 80 2>/dev/null | tail -1 | python3 -m json.tool || echo "(device unreachable)"
