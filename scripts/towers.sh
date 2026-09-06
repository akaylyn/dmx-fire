#!/usr/bin/env bash
# Per-tower config + live DMX bytes, in one table.
#
# Answers "why is tower N dark?" without reading raw JSON. Prints each tower's
# persisted config alongside the bytes actually on the wire for its decoder and
# uplight blocks, and flags any setting that blanks a tower.
#
#   scripts/towers.sh
#   DMXFIRE_HOST=10.0.0.42 scripts/towers.sh
#
# Requires the workstation to be on the device's WiFi AP (or DMXFIRE_HOST set).

set -uo pipefail
HOST="${DMXFIRE_HOST:-192.168.4.1}"
HOST="${HOST#http://}"
HOST="${HOST%%/}"

fetch() {
  # curl first; fall back to raw HTTP/1.0 over nc, which is what scripts/debug.sh
  # uses — the ESP32 WebServer does not always cope with curl's default headers.
  curl -s -m 6 "http://${HOST}/api/state" 2>/dev/null && return 0
  (printf "GET /api/state HTTP/1.0\r\nHost: %s\r\n\r\n" "$HOST"; sleep 1) \
    | nc -w 4 "$HOST" 80 2>/dev/null | tail -1
}

JSON="$(fetch)"

if [ -z "${JSON// }" ]; then
  echo "Device unreachable at ${HOST}."
  echo
  echo "  - Is the workstation joined to the device's WiFi AP?"
  echo "  - Is the device powered and past boot?"
  echo "  - Override the address with DMXFIRE_HOST=<ip> $0"
  exit 1
fi

printf '%s' "$JSON" | python3 -c '
import json, sys

raw = sys.stdin.read().strip()
try:
    d = json.loads(raw)
except json.JSONDecodeError as e:
    print("Reply was not JSON (%s).\nFirst 200 bytes:\n%s" % (e, raw[:200]))
    sys.exit(1)

towers = d.get("towers") or []
ch     = (d.get("dmx") or {}).get("ch") or []

def block(start, n=4):
    """DMX channels are 1-indexed; ch[] is 0-indexed."""
    if len(ch) < start - 1 + n:
        return ["?"] * n
    return ch[start - 1 : start - 1 + n]

fsm = d.get("fsm", d.get("state", "?"))
if isinstance(fsm, dict):
    fsm = fsm.get("state", "?")
conf = d.get("confluence") or {}
print("FSM %s   purge=%s   channels=%d   confluence conn=%s fire=%s" % (
    fsm, d.get("purge", "?"), len(ch),
    conf.get("connected", "?"), conf.get("fireEnabled", "?")))
print()
hdr = "%-6s %-6s %-6s %-13s %5s %6s   %-16s %-16s" % (
    "tower", "conn", "fire", "theme", "brt", "speed", "decoder R/G/B/FIRE", "uplight R/G/B/W")
print(hdr)
print("-" * len(hdr))

problems = []
for i, t in enumerate(towers):
    base = 4 + i * 15          # towers.cpp: CHANNELS_PER_TOWER = 15
    dec  = block(base + 1)     # strips R/G/B + fire valve on CH4
    up   = block(base + 5)     # uplight R/G/B/W
    conn = t.get("connected")
    brt  = t.get("brightness")
    fire = t.get("fireEnabled")
    print("%-6d %-6s %-6s %-13s %5s %6s   %-16s %-16s" % (
        i, conn, fire, t.get("theme"), brt, t.get("speed"),
        "/".join(str(x) for x in dec), "/".join(str(x) for x in up)))

    label = "tower %d" % i
    if conn is False:
        problems.append("%s: connected=false -> towerWrite() is skipped entirely, so the "
                        "decoder AND uplight both stay dark. The valve is still driven "
                        "shut, so this cannot strand it open." % label)
    if brt == 0:
        problems.append("%s: brightness=0 -> themeRender() bakes brightness into the "
                        "strip RGB, so the strips are black. The uplight still lights "
                        "during a fire because applyFireLook() writes the global fire "
                        "colour and ignores brightness." % label)
    if fire is False:
        problems.append("%s: fireEnabled=false -> the valve on this tower is held shut. "
                        "The lights keep running, so the fixture looks healthy while its "
                        "propane is isolated." % label)

if conf.get("connected") is False:
    problems.append("confluence: connected=false -> the central solenoid (CH1) is driven "
                    "shut and never fires.")
if conf.get("fireEnabled") is False:
    problems.append("confluence: fireEnabled=false -> the central solenoid is isolated; "
                    "the tower valves can still open.")

print()
# Mirrors VALVE_CHANNELS in Test_Button_DMX/dmx.h.
VALVES = (1, 8, 23, 38, 53)
print("valve channels  " + "  ".join(
    "CH%d=%s" % (c, ch[c - 1] if len(ch) >= c else "?") for c in VALVES))

# A solenoid channel carries 0 or 255 and nothing else — dmxShadowWrite() refuses
# anything in between. A mid-scale byte here is a firmware fault, not a setting.
for c in VALVES:
    if len(ch) >= c and ch[c - 1] not in (0, 255):
        problems.append("CH%d = %d -> a valve channel must be 0 or 255. The binary guard "
                        "in dmx.cpp has been bypassed." % (c, ch[c - 1]))

if problems:
    print()
    print("FLAGGED")
    for p in problems:
        print("  * " + p)
else:
    print()
    print("No blanking config found on any tower.")
'
