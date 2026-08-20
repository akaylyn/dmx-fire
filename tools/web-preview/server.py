#!/usr/bin/env python3
"""
Local mock server for the DMX Fire web UI.

Serves tools/web-preview/index.html (the canonical source of truth) and mocks
every HTTP endpoint that the firmware's web.cpp exposes, so the UI can be
clicked through and verified in a normal browser without flashing the device.

Endpoints (all log to stdout in the same format as the firmware's [WEB] LOG_I):
    GET  /                      -> index.html (re-read per request; live edits)
    POST /set                   -> update in-memory config, 200 OK
    GET  /api/state             -> JSON snapshot mimicking the device
    POST /api/button/press      -> sim FSM into FIRE_ACTIVE
    POST /api/button/release    -> sim FSM into END_CUE -> COOLDOWN -> IDLE
    POST /api/button/reset      -> force FSM IDLE
    POST /api/morse             -> simulate morse playback start
    POST /api/morse/stop        -> simulate morse playback stop
    GET  /__preview/mtime       -> mtime of index.html (used by injected
                                   reload poller — see RELOAD_SNIPPET below)

Binds to localhost only (security: this is a debug tool, not exposed to LAN).
"""

from __future__ import annotations

import json
import math
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs

PORT = 8123
HOST = "127.0.0.1"
HERE = Path(__file__).resolve().parent
INDEX_HTML = HERE / "index.html"
SIMULATOR_HTML = HERE / "simulator.html"  # dev-only animation preview, not part of firmware UI

# Injected into every served HTML response. Polls /__preview/mtime every 500 ms
# and reloads the page when index.html changes on disk, so editing the file in
# your editor refreshes the browser automatically.
RELOAD_SNIPPET = b"""<script>
(function () {
  var current = null;
  setInterval(function () {
    fetch('/__preview/mtime').then(function (r) { return r.text(); }).then(function (t) {
      if (current === null) { current = t; return; }
      if (t !== current) { current = t; location.reload(); }
    }).catch(function () {});
  }, 500);
})();
</script>
"""

# Match firmware defaults (Test_Button_DMX/storage.cpp).
# `boot_id` is a per-process random fingerprint. The web UI persists the "Test
# Fire armed" state alongside this id; when the device reboots (or this mock is
# restarted) the id changes and the browser re-closes the arming cover.
STATE = {
    # Mirrors the audio block in handleApiState(). build_state_json() animates the
    # live values so the Audio tab's meters and beat blinker work against the mock.
    "audio": {
        "armed": False, "fresh": False, "peer": "0.0.0.0", "port": 4210,
        "ageMs": -1, "pps": 0, "packets": 0, "gaps": 0, "bad": 0, "floods": 0,
        "bass": 0, "mid": 0, "treble": 0, "level": 0, "bpm": 0, "beatMs": 0,
        "beat": False, "confident": False, "shotActive": False,
        "dutyUsedMs": 0, "dutyCapMs": 3000,
        "cfg": {
            "shotMs": 150, "minGapMs": 200, "dutyPct": 40, "dutyWinMs": 10000,
            "maxOpenMs": 1000, "leadMs": 120, "staleMs": 500,
            "bassOn": 170, "bassOff": 140, "beatMin": 90, "dropMin": 200,
            "dropGapMs": 3000, "dropShotMs": 400, "lightMode": 1, "lightDepth": 150,
        },
    },
    "boot_id": uuid.uuid4().hex,
    "boot_ms": int(time.time() * 1000),
    "fsm": {"state": "IDLE", "since_ms": 0},
    "button": {
        "mode": 0,
        "fireDurationMs": 3000,
        "cooldownMs": 10000,
        "endCuePattern": 0,
        "endCueMs": 1000,
        "machineGunBurstMs": 200,
    },
    # Uplight colour held while any valve is open. Global, not per tower.
    "fireUplight": {"r": 255, "g": 110, "b": 0, "w": 0},
    "confluence": {"connected": True, "fireLevel": 255},
    # `speed` is a percentage where 100 = normal. Scales time-based theme
    # behaviour (flash cycle, Simon beat, rainbow hue rotation, candle flicker).
    "towers": [
        {"connected": True, "theme": "green", "brightness": 128, "speed": 100, "flameLevel": 255},
        {"connected": True, "theme": "green", "brightness": 128, "speed": 100, "flameLevel": 255},
        {"connected": True, "theme": "green", "brightness": 128, "speed": 100, "flameLevel": 255},
        {"connected": True, "theme": "green", "brightness": 128, "speed": 100, "flameLevel": 255},
    ],
    "morse": {"unitMs": 150, "playing": False, "text": ""},
}
LOCK = threading.Lock()


def log(msg: str) -> None:
    print(f"[WEB] {msg}", flush=True)


def set_fsm(state: str) -> None:
    STATE["fsm"] = {"state": state, "since_ms": int(time.time() * 1000)}


def schedule(delay_ms: int, fn) -> None:
    threading.Timer(delay_ms / 1000.0, fn).start()


def simulate_fire_cycle() -> None:
    """Mimic the firmware FSM: FIRE_ACTIVE -> END_CUE -> COOLDOWN -> IDLE."""
    fire_ms = STATE["button"]["fireDurationMs"]
    cool_ms = STATE["button"]["cooldownMs"]
    end_cue_ms = STATE["button"]["endCueMs"]
    with LOCK:
        set_fsm("FIRE_ACTIVE")

    def to_end_cue():
        # endCueMs == 0 skips END_CUE entirely, same as the firmware FSM.
        if end_cue_ms == 0:
            with LOCK:
                if STATE["fsm"]["state"] == "FIRE_ACTIVE":
                    set_fsm("COOLDOWN")
            schedule(cool_ms, to_idle)
            return
        with LOCK:
            if STATE["fsm"]["state"] == "FIRE_ACTIVE":
                set_fsm("END_CUE")
        schedule(end_cue_ms, to_cooldown)

    def to_cooldown():
        with LOCK:
            if STATE["fsm"]["state"] == "END_CUE":
                set_fsm("COOLDOWN")
        schedule(cool_ms, to_idle)

    def to_idle():
        with LOCK:
            if STATE["fsm"]["state"] == "COOLDOWN":
                set_fsm("IDLE")

    schedule(fire_ms, to_end_cue)


def update_config(target: str, args: dict[str, str]) -> None:
    """Apply a /set form submission to STATE — mirrors handleSet() in web.cpp."""
    if target == "audio":
        cfg = STATE["audio"]["cfg"]
        for k, v in args.items():
            if k == "target":
                continue
            # Web field names are audXxx; the JSON keys drop the prefix.
            key = k[3].lower() + k[4:] if k.startswith("aud") and len(k) > 3 else k
            if key in cfg:
                cfg[key] = int(v)
        if cfg["bassOff"] >= cfg["bassOn"]:
            cfg["bassOff"] = max(0, cfg["bassOn"] - 8)
        return

    if target == "button":
        STATE["button"].update(
            mode=int(args.get("mode", 0)),
            fireDurationMs=int(args.get("fireDurationMs", 3000)),
            cooldownMs=int(args.get("cooldownMs", 10000)),
            endCuePattern=int(args.get("endCuePattern", 0)),
            endCueMs=int(args.get("endCueMs", 1000)),
            machineGunBurstMs=int(args.get("machineGunBurstMs", 200)),
        )
    elif target == "fireup":
        colour = args.get("fireUpColor", "")
        if len(colour) == 7 and colour[0] == "#":
            STATE["fireUplight"].update(
                r=int(colour[1:3], 16), g=int(colour[3:5], 16), b=int(colour[5:7], 16)
            )
        STATE["fireUplight"]["w"] = int(args.get("fireUpW", 0))
    elif target == "confluence":
        STATE["confluence"]["connected"] = "connected" in args
        STATE["confluence"]["fireLevel"] = int(args.get("fireLevel", 0))
    elif target == "all":
        for t in STATE["towers"]:
            t["theme"] = args.get("theme", "green")
            t["brightness"] = int(args.get("brightness", 128))
            t["speed"] = int(args.get("speed", 100))
            t["flameLevel"] = int(args.get("flameLevel", 255))
    else:
        try:
            idx = int(target)
        except ValueError:
            return
        if 0 <= idx < len(STATE["towers"]):
            STATE["towers"][idx]["connected"] = "connected" in args
            STATE["towers"][idx]["theme"] = args.get("theme", "green")
            STATE["towers"][idx]["brightness"] = int(args.get("brightness", 128))
            STATE["towers"][idx]["speed"] = int(args.get("speed", 100))
            STATE["towers"][idx]["flameLevel"] = int(args.get("flameLevel", 255))


def build_state_json() -> bytes:
    """JSON shape mimics handleApiState() in web.cpp — same field names."""
    # Synthetic 120 BPM source so the meters, beat blinker and budget bar move.
    # The firmware derives these from real packets; here they are a function of time.
    a = STATE["audio"]
    if a["armed"] or a["fresh"]:
        t = time.time()
        phase = (t % 0.5) / 0.5
        env = math.exp(-phase * 4.0)
        a["bass"] = int(255 * env)
        a["mid"] = int(120 + 100 * math.sin(t * 2.1))
        a["treble"] = int(80 + 70 * math.sin(t * 5.3))
        a["level"] = max(a["bass"], a["mid"], a["treble"])
        a["bpm"], a["beatMs"] = 120, 500
        a["beat"] = phase < 0.24
        a["confident"] = a["fresh"] = True
        a["pps"], a["ageMs"] = 40, 12
        a["packets"] += 1
        a["peer"] = "192.168.4.7"
        cap = min(a["cfg"]["dutyWinMs"] * a["cfg"]["dutyPct"] // 100, 3000)
        a["dutyCapMs"] = cap
        a["dutyUsedMs"] = int(cap * (1 + math.sin(t * 0.4)) / 4)

    now = int(time.time() * 1000)
    fsm = STATE["fsm"]
    payload = {
        "boot_id": STATE["boot_id"],
        "uptime_ms": now - STATE["boot_ms"],
        "fsm": {"state": fsm["state"], "elapsed_ms": now - fsm["since_ms"]},
        "button": STATE["button"],
        "fireUplight": STATE["fireUplight"],
        "ota": {"inProgress": False, "lastError": ""},
        "confluence": STATE["confluence"],
        "towers": STATE["towers"],
        "morse": STATE["morse"],
        # DMX shadow buffer placeholder (64 zero-filled channels). Real device
        # fills these from dmxLastFrame[] — for UI verification, zeroes are fine.
        # Inserted before dmx, matching handleApiState()'s ordering in web.cpp.
        "audio": STATE["audio"],
        "dmx": {"ch": [0] * 64},
    }
    return json.dumps(payload).encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    # Quiet the default access-log line; we print our own [WEB] lines.
    def log_message(self, fmt, *args):  # noqa: N802
        return

    def _send(self, status: int, body: bytes = b"", ctype: str = "text/plain") -> None:
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _read_form(self) -> dict[str, str]:
        length = int(self.headers.get("Content-Length", "0") or 0)
        raw = self.rfile.read(length).decode("utf-8") if length else ""
        # Browsers send multipart/form-data for FormData by default; the firmware
        # also accepts urlencoded. Handle the simple urlencoded path first.
        ctype = self.headers.get("Content-Type", "")
        if "application/x-www-form-urlencoded" in ctype or "=" in raw and "boundary=" not in ctype:
            parsed = parse_qs(raw, keep_blank_values=True)
            return {k: v[0] for k, v in parsed.items()}
        # Minimal multipart parser — just enough to extract name="..." fields.
        out: dict[str, str] = {}
        if "boundary=" in ctype:
            boundary = ctype.split("boundary=", 1)[1].strip()
            sep = f"--{boundary}"
            for part in raw.split(sep):
                if "Content-Disposition" not in part:
                    continue
                # name="foo"
                try:
                    name = part.split('name="', 1)[1].split('"', 1)[0]
                except IndexError:
                    continue
                # body is after the blank line
                try:
                    value = part.split("\r\n\r\n", 1)[1].rsplit("\r\n", 1)[0]
                except IndexError:
                    value = ""
                out[name] = value
        return out

    def do_GET(self):  # noqa: N802
        if self.path == "/" or self.path.startswith("/?"):
            log(f"GET /  client={self.client_address[0]}")
            try:
                body = INDEX_HTML.read_bytes()
            except FileNotFoundError:
                self._send(500, b"index.html missing", "text/plain")
                return
            # Inject the live-reload poller just before </body> (case-insensitive).
            lower = body.lower()
            idx = lower.rfind(b"</body>")
            if idx >= 0:
                body = body[:idx] + RELOAD_SNIPPET + body[idx:]
            else:
                body = body + RELOAD_SNIPPET
            self._send(200, body, "text/html; charset=utf-8")
        elif self.path == "/__preview/mtime":
            # Aggregate mtime so the live-reload poller fires for either the
            # firmware UI preview OR the simulator (whichever is open).
            try:
                m1 = INDEX_HTML.stat().st_mtime_ns
            except FileNotFoundError:
                m1 = 0
            try:
                m2 = SIMULATOR_HTML.stat().st_mtime_ns
            except FileNotFoundError:
                m2 = 0
            self._send(200, str(max(m1, m2)).encode("ascii"), "text/plain")
        elif self.path == "/simulator" or self.path.startswith("/simulator?"):
            log(f"GET /simulator  client={self.client_address[0]}")
            try:
                body = SIMULATOR_HTML.read_bytes()
            except FileNotFoundError:
                self._send(500, b"simulator.html missing", "text/plain")
                return
            # Inject the same live-reload poller so simulator edits also refresh.
            lower = body.lower()
            idx = lower.rfind(b"</body>")
            if idx >= 0:
                body = body[:idx] + RELOAD_SNIPPET + body[idx:]
            else:
                body = body + RELOAD_SNIPPET
            self._send(200, body, "text/html; charset=utf-8")
        elif self.path == "/api/state":
            with LOCK:
                body = build_state_json()
            self._send(200, body, "application/json")
        else:
            # Mimic captive-portal behaviour: any unknown path 302s to /.
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()

    def do_POST(self):  # noqa: N802
        # /api/update carries a BINARY firmware image. It must be routed before
        # _read_form(), which reads the whole body and utf-8 decodes it — that
        # throws on binary and the request dies with no response at all.
        if self.path == "/api/update":
            self._handle_ota()
            return

        args = self._read_form()
        if self.path == "/set":
            target = args.get("target", "?")
            extras = " ".join(f"{k}={v}" for k, v in args.items() if k != "target")
            log(f"POST /set  target={target}  {extras}")
            with LOCK:
                update_config(target, args)
            self._send(200)
        elif self.path == "/api/button/press":
            log("POST /api/button/press")
            simulate_fire_cycle()
            self._send(200)
        elif self.path == "/api/button/release":
            log("POST /api/button/release")
            # In FIREBALL mode the cycle runs to completion regardless; in
            # PARTY/MACHINE_GUN the device would short-circuit. For UI verification
            # we let the cycle continue — toggle in-place if you need party-mode sim.
            self._send(200)
        elif self.path == "/api/button/reset":
            log("POST /api/button/reset")
            with LOCK:
                set_fsm("IDLE")
            self._send(200)
        elif self.path == "/api/morse":
            text = args.get("text", "")
            unit = int(args.get("unitMs", STATE["morse"]["unitMs"]))
            log(f"POST /api/morse  text='{text}' unit={unit}")
            with LOCK:
                STATE["morse"].update(playing=True, text=text, unitMs=unit)
            # Auto-clear after ~text length * unit * 5 (loose estimate).
            duration = max(500, len(text) * unit * 5)

            def clear():
                with LOCK:
                    STATE["morse"]["playing"] = False
            schedule(duration, clear)
            self._send(200, b"OK", "text/plain")
        elif self.path == "/api/morse/stop":
            log("POST /api/morse/stop")
            with LOCK:
                STATE["morse"]["playing"] = False
            self._send(200)
        elif self.path == "/api/captive/dismiss":
            log("POST /api/captive/dismiss")
            self._send(204)
        elif self.path == "/api/audio/arm":
            log("POST /api/audio/arm")
            with LOCK:
                STATE["audio"]["armed"] = True
                STATE["audio"]["fresh"] = True
            self._send(200)
        elif self.path == "/api/audio/disarm":
            log("POST /api/audio/disarm")
            with LOCK:
                STATE["audio"]["armed"] = False
            self._send(200)
        elif self.path == "/api/purge/start":
            log("POST /api/purge/start")
            with LOCK:
                STATE["purge"] = True
            self._send(200)
        elif self.path == "/api/purge/stop":
            log("POST /api/purge/stop")
            with LOCK:
                STATE["purge"] = False
            self._send(200)
        else:
            self._send(404, b"not found")

    def _handle_ota(self) -> None:
        """Mock OTA upload — mirrors the firmware's gating in ota.cpp.

        Drains the body so the browser's upload-progress bar runs for real,
        then refuses unless the rig is idle, exactly as the device does.
        """
        length = int(self.headers.get("Content-Length", 0))
        remaining = length
        while remaining > 0:
            chunk = self.rfile.read(min(65536, remaining))
            if not chunk:
                break  # EOF — without this, read() returning b"" spins forever
            remaining -= len(chunk)
        with LOCK:
            fsm = STATE["fsm"]["state"]
        log(f"POST /api/update ({length} bytes, fsm={fsm})")
        if fsm != "IDLE":
            self._send(
                500,
                json.dumps({"ok": False, "error": f"FSM is {fsm}, must be IDLE"}).encode(),
                "application/json",
            )
        else:
            self._send(
                200, json.dumps({"ok": True, "bytes": length}).encode(), "application/json"
            )


def main() -> None:
    if not INDEX_HTML.exists():
        raise SystemExit(f"index.html not found at {INDEX_HTML}")
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    log(f"local DMX Fire preview at http://{HOST}:{PORT}/")
    log(f"serving {INDEX_HTML}")
    log("Ctrl-C to stop")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("stopped")


if __name__ == "__main__":
    main()
