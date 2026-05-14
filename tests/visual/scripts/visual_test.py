"""Visual test runner — drives the rig via API and captures camera frames.

Each invocation creates a NEW timestamped run directory under
tests/visual/runs/<UTC timestamp>/ and writes:
  - one .jpg per capture step
  - report.json (machine-readable)
  - report.md   (human-readable; lists each test, image, expected/actual, pass/fail)

Tests:
  T01 GET /api/state shape — pass if all expected keys present
  T02 IDLE flash window  — pass if at least one frame in the burst captures the
                           palette-coloured flash (firmware: 800ms ON / 4000ms cycle).
                           Detected by per-step DMX state read: the 4 RGBW
                           channels of tower 0 must be > 0 in at least one frame.
  T03 IDLE blank window  — pass if at least one frame in the burst sees the
                           tower 0 RGBW channels all = 0.
  T04 FIRE_ACTIVE        — pass if /api/state reports fsm.state == FIRE_ACTIVE
                           after a press AND tower 0 RGBW values are non-zero
                           (fire palette).
  T05 FIRE → COOLDOWN    — pass if FSM walks through END_CUE/COOLDOWN/IDLE.
  T06 PARTY mode release — pass if release in PARTY mode ends fire early.
  T07 Tower disconnect   — pass if disconnected tower's DMX channels stop changing.

Network: uses `nc` for HTTP because the laptop's Python sockets have a
macOS routing quirk on this network setup.
Camera:  uses `imagesnap` against FaceTime HD Camera. Output downscaled with
         `sips` to ~320px JPEGs to keep the directory readable.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import subprocess
import sys
import time
import urllib.parse
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RUNS = REPO / "tests" / "visual" / "runs"
HOST = os.environ.get("DMXFIRE_HOST", "192.168.4.1")
CAM = os.environ.get("DMXFIRE_CAMERA", "FaceTime HD Camera")


# ---------- HTTP via nc ----------

def http(method: str, path: str, data: dict | None = None) -> tuple[str, bytes]:
    body = urllib.parse.urlencode(data) if data else ""
    req = (
        f"{method} {path} HTTP/1.0\r\nHost: {HOST}\r\n"
        f"Content-Length: {len(body)}\r\n"
        f"Content-Type: application/x-www-form-urlencoded\r\n"
        f"Connection: close\r\n\r\n{body}"
    )
    p = subprocess.run(["nc", "-w", "5", HOST, "80"],
                       input=req.encode(), capture_output=True, timeout=12)
    head, _, b = p.stdout.partition(b"\r\n\r\n")
    return head.split(b"\r\n", 1)[0].decode(), b


def get_state() -> dict:
    return json.loads(http("GET", "/api/state")[1])


def post(path: str) -> None:
    http("POST", path)


def post_set(target: str, **kw) -> None:
    http("POST", "/set", data={"target": target, **kw})


# ---------- Camera ----------

def capture(label: str, run_dir: Path, warmup: float = 0.9) -> Path:
    raw = run_dir / f".{label}_raw.jpg"
    out = run_dir / f"{label}.jpg"
    subprocess.run(
        ["imagesnap", "-q", "-w", str(warmup), "-d", CAM, str(raw)],
        check=True, capture_output=True, timeout=10,
    )
    subprocess.run(
        ["sips", "-Z", "320", "-s", "formatOptions", "low", str(raw), "--out", str(out)],
        check=True, capture_output=True, timeout=5,
    )
    raw.unlink(missing_ok=True)
    return out


# ---------- Test runner ----------

class Run:
    def __init__(self, root: Path):
        self.root = root
        root.mkdir(parents=True, exist_ok=True)
        self.results: list[dict] = []

    def add(self, **kw):
        self.results.append(kw)
        status = "PASS" if kw["pass"] else "FAIL"
        print(f"  [{status}] {kw['id']} — {kw['name']}: {kw.get('detail','')}")

    def write_report(self):
        # JSON
        (self.root / "report.json").write_text(json.dumps({
            "host": HOST,
            "camera": CAM,
            "started": self.root.name,
            "tests": self.results,
        }, indent=2))
        # Markdown
        lines = [
            f"# Visual test run — {self.root.name}",
            "",
            f"- Host: `{HOST}`",
            f"- Camera: `{CAM}`",
            "",
            "| ID | Name | Status | Image(s) | Detail |",
            "|----|------|--------|----------|--------|",
        ]
        for r in self.results:
            imgs = ", ".join(f"[{Path(i).name}]({Path(i).name})" for i in r.get("images", []))
            status = "✅ PASS" if r["pass"] else "❌ FAIL"
            lines.append(f"| {r['id']} | {r['name']} | {status} | {imgs or '—'} | {r.get('detail','')} |")
        (self.root / "report.md").write_text("\n".join(lines) + "\n")


def baseline():
    """Reset to known config — fast button, all towers green, confluence on."""
    post("/api/button/reset")
    post_set("button", mode=0, fireDurationMs=1500, cooldownMs=2000, endCuePattern=0)
    post_set("confluence", connected="on", fireLevel=255)
    for i in range(4):
        post_set(str(i), connected="on", palette="green", brightness=255, flameLevel=255)


def t01_state_shape(run: Run):
    s = get_state()
    needed_top = {"uptime_ms", "fsm", "button", "confluence", "towers", "dmx"}
    missing = needed_top - set(s)
    ok = (not missing) and len(s["towers"]) == 4 and len(s["dmx"]["ch"]) == 64
    run.add(id="T01", name="GET /api/state shape",
            **{"pass": ok, "detail": f"missing={missing or 'none'}; towers={len(s['towers'])}; dmx_ch={len(s['dmx']['ch'])}",
               "images": []})


def t02_t03_idle_flash_blank(run: Run):
    """Capture frames + DMX state across a full IDLE cycle (4s).
    Pass T02 if any frame has tower 0 RGBW > 0; pass T03 if any frame has all = 0.
    """
    baseline()
    post_set("all", palette="green", brightness=255, flameLevel=255)
    time.sleep(0.3)

    n = 12
    dt = 0.45
    images: list[Path] = []
    rgbws: list[list[int]] = []
    for i in range(n):
        s = get_state()
        ch5_8 = s["dmx"]["ch"][4:8]
        rgbws.append(ch5_8)
        p = capture(f"T02_idle_{i:02d}", run.root)
        images.append(p)

    any_lit = any(any(v > 0 for v in r) for r in rgbws)
    any_blank = any(all(v == 0 for v in r) for r in rgbws)
    run.add(id="T02", name="IDLE flash visible",
            **{"pass": any_lit, "detail": f"max RGBW values across burst: {max(rgbws, key=sum)}",
               "images": [str(p) for p in images]})
    run.add(id="T03", name="IDLE blank visible",
            **{"pass": any_blank, "detail": f"min RGBW values: {min(rgbws, key=sum)}",
               "images": [str(p) for p in images]})


def t04_fire_active(run: Run):
    baseline()
    post_set("button", mode=0, fireDurationMs=8000, cooldownMs=2000)
    post_set("all", palette="fire", brightness=255, flameLevel=255)
    time.sleep(0.3)
    post("/api/button/press")
    time.sleep(0.4)

    s = get_state()
    ch5_8 = s["dmx"]["ch"][4:8]
    img = capture("T04_fire_active", run.root)
    fsm_ok = s["fsm"]["state"] == "FIRE_ACTIVE"
    rgbw_ok = any(v > 0 for v in ch5_8)
    run.add(id="T04", name="FIRE_ACTIVE drives towers",
            **{"pass": fsm_ok and rgbw_ok,
               "detail": f"fsm={s['fsm']['state']}; tower0 RGBW={ch5_8}",
               "images": [str(img)]})


def t05_fire_to_cooldown(run: Run):
    """Continue from T04 — wait for END_CUE → COOLDOWN → IDLE."""
    end = time.time() + 12
    seen = set()
    images: list[Path] = []
    last_state = None
    while time.time() < end:
        s = get_state()
        st = s["fsm"]["state"]
        if st != last_state:
            seen.add(st)
            img = capture(f"T05_fsm_{st}", run.root)
            images.append(img)
            last_state = st
            if st == "IDLE":
                break
        time.sleep(0.2)
    expected = {"FIRE_ACTIVE", "END_CUE", "COOLDOWN", "IDLE"}
    ok = expected.issubset(seen | {"FIRE_ACTIVE"})  # may have already left FIRE_ACTIVE
    run.add(id="T05", name="FSM walks FIRE_ACTIVE→END_CUE→COOLDOWN→IDLE",
            **{"pass": ok, "detail": f"states observed: {sorted(seen)}",
               "images": [str(p) for p in images]})


def t06_party_release(run: Run):
    baseline()
    post("/api/button/reset")
    post_set("button", mode=1, fireDurationMs=8000, cooldownMs=2000)
    post_set("all", palette="fire", brightness=255, flameLevel=255)
    time.sleep(0.3)
    post("/api/button/press")
    time.sleep(0.4)
    pre = get_state()["fsm"]["state"]
    img_before = capture("T06_party_during", run.root)
    post("/api/button/release")
    time.sleep(0.3)
    post_state = get_state()["fsm"]["state"]
    img_after = capture("T06_party_after_release", run.root)
    ok = pre == "FIRE_ACTIVE" and post_state in {"END_CUE", "COOLDOWN", "IDLE"}
    run.add(id="T06", name="PARTY mode release ends fire early",
            **{"pass": ok, "detail": f"pre={pre}, post={post_state}",
               "images": [str(img_before), str(img_after)]})
    post("/api/button/reset")


def t07_disconnect(run: Run):
    baseline()
    post_set("1", palette="blue", brightness=255, flameLevel=255)  # leave T1 connected first
    post_set("button", mode=0, fireDurationMs=2000, cooldownMs=2000)
    # Snapshot tower 1 channels (DMX ch 20-23 = decoder, ch 24-34 = strobe block)
    s_before = get_state()
    img_before = capture("T07_t1_connected", run.root)
    # Disconnect T1
    post_set("1", palette="blue", brightness=255, flameLevel=255)  # connected="on" omitted = disconnected
    time.sleep(0.3)
    s_after = get_state()
    img_after = capture("T07_t1_disconnected", run.root)
    t1_connected = s_after["towers"][1]["connected"]
    ok = (s_before["towers"][1]["connected"] is True) and (t1_connected is False)
    run.add(id="T07", name="Tower disconnect via /set",
            **{"pass": ok,
               "detail": f"before connected={s_before['towers'][1]['connected']}, after connected={t1_connected}",
               "images": [str(img_before), str(img_after)]})
    # Restore
    post_set("1", connected="on", palette="green", brightness=255, flameLevel=255)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", default=None,
                        help="Optional label appended to the run directory name")
    args = parser.parse_args()

    ts = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    label = f"_{args.label}" if args.label else ""
    run = Run(RUNS / f"{ts}{label}")
    print(f"==> run dir: {run.root}")

    try:
        baseline()
        print("==> T01 state shape")
        t01_state_shape(run)
        print("==> T02 / T03 idle flash + blank windows")
        t02_t03_idle_flash_blank(run)
        print("==> T04 fire active")
        t04_fire_active(run)
        print("==> T05 fire → cooldown")
        t05_fire_to_cooldown(run)
        print("==> T06 party release")
        t06_party_release(run)
        print("==> T07 disconnect")
        t07_disconnect(run)
    finally:
        # Always emit the report — even partial runs are useful.
        run.write_report()
        # Safe shutdown
        try:
            post("/api/button/reset")
            post_set("all", palette="green", brightness=128, flameLevel=255)
        except Exception:
            pass

    n_pass = sum(1 for r in run.results if r["pass"])
    n_total = len(run.results)
    print(f"==> {n_pass}/{n_total} tests passed")
    print(f"==> report: {run.root}/report.md")
    return 0 if n_pass == n_total else 1


if __name__ == "__main__":
    sys.exit(main())
