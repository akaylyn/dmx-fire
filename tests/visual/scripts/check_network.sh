#!/usr/bin/env bash
# Verify that the laptop is on the dmx-fire AP and the device responds to API.
# Prints PASS/FAIL and exits non-zero on failure so callers can branch.
#
# Usage: tests/visual/scripts/check_network.sh [host]
#   host defaults to 192.168.4.1

HOST="${1:-192.168.4.1}"

# Ping check (uses raw ICMP, bypasses macOS routing weirdness)
if ! ping -c 1 -W 2000 -t 2 "$HOST" >/dev/null 2>&1; then
  echo "FAIL: $HOST did not respond to ping"
  echo "      en0: $(ifconfig en0 | awk '/inet / {print $2}')"
  echo "      Is the laptop joined to the dmx-fire AP?"
  exit 1
fi

# HTTP API check (uses nc — Python sockets have a macOS routing quirk)
RESPONSE=$( (printf "GET /api/state HTTP/1.0\r\nHost: %s\r\n\r\n" "$HOST"; sleep 1) \
  | nc -w 4 "$HOST" 80 | head -1 )

if [[ "$RESPONSE" != *"200 OK"* ]]; then
  echo "FAIL: $HOST/api/state responded: $RESPONSE"
  exit 2
fi

echo "PASS: $HOST reachable, /api/state returned: $RESPONSE"
