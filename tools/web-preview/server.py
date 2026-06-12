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
    "boot_id": uuid.uuid4().hex,
    "boot_ms": int(time.time() * 1000),
    "fsm": {"state": "IDLE", "since_ms": 0},
    "button": {
        "mode": 0,
        "fireDurationMs": 3000,
        "cooldownMs": 10000,
        "endCuePattern": 0,
        "machineGunBurstMs": 200,
    },
    "confluence": {"connected": True, "fireLevel": 255},
    "towers": [
        {"connected": True, "palette": "green", "brightness": 128, "flameLevel": 255},
        {"connected": True, "palette": "green", "brightness": 128, "flameLevel": 255},
        {"connected": True, "palette": "green", "brightness": 128, "flameLevel": 255},
        {"connected": True, "palette": "green", "brightness": 128, "flameLevel": 255},
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
    end_cue_ms = 300
    with LOCK:
        set_fsm("FIRE_ACTIVE")

    def to_end_cue():
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
    if target == "button":
        STATE["button"].update(
            mode=int(args.get("mode", 0)),
            fireDurationMs=int(args.get("fireDurationMs", 3000)),
            cooldownMs=int(args.get("cooldownMs", 10000)),
            endCuePattern=int(args.get("endCuePattern", 0)),
            machineGunBurstMs=int(args.get("machineGunBurstMs", 200)),
        )
    elif target == "confluence":
        STATE["confluence"]["connected"] = "connected" in args
        STATE["confluence"]["fireLevel"] = int(args.get("fireLevel", 0))
    elif target == "all":
        for t in STATE["towers"]:
            t["palette"] = args.get("palette", "green")
            t["brightness"] = int(args.get("brightness", 128))
            t["flameLevel"] = int(args.get("flameLevel", 255))
    else:
        try:
            idx = int(target)
        except ValueError:
            return
        if 0 <= idx < len(STATE["towers"]):
            STATE["towers"][idx]["connected"] = "connected" in args
            STATE["towers"][idx]["palette"] = args.get("palette", "green")
            STATE["towers"][idx]["brightness"] = int(args.get("brightness", 128))
            STATE["towers"][idx]["flameLevel"] = int(args.get("flameLevel", 255))


def build_state_json() -> bytes:
    """JSON shape mimics handleApiState() in web.cpp — same field names."""
    now = int(time.time() * 1000)
    fsm = STATE["fsm"]
    payload = {
        "boot_id": STATE["boot_id"],
        "uptime_ms": now - STATE["boot_ms"],
        "fsm": {"state": fsm["state"], "elapsed_ms": now - fsm["since_ms"]},
        "button": STATE["button"],
        "confluence": STATE["confluence"],
        "towers": STATE["towers"],
        "morse": STATE["morse"],
        # DMX shadow buffer placeholder (64 zero-filled channels). Real device
        # fills these from dmxLastFrame[] — for UI verification, zeroes are fine.
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
            try:
                mtime = INDEX_HTML.stat().st_mtime_ns
            except FileNotFoundError:
                mtime = 0
            self._send(200, str(mtime).encode("ascii"), "text/plain")
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
        else:
            self._send(404, b"not found")


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
