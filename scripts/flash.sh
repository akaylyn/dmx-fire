#!/usr/bin/env bash
# Compile and upload firmware to the M5AtomS3.
# Pass --erase to wipe flash first (use if device is in a boot-loop).
#
# Usage:
#   scripts/flash.sh
#   scripts/flash.sh --erase
REPO="$(cd "$(dirname "$0")/.." && pwd)"
exec "$REPO/tests/visual/scripts/flash.sh" "$@"
