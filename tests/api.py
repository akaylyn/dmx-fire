"""HTTP client for the DMX-Fire ESP32. Thin wrapper around requests."""

from __future__ import annotations

import time
from typing import Any

import json
import os

import requests
import urllib3


class DeviceTimeout(AssertionError):
    pass


def _local_addr_for(host: str) -> str | None:
    """Find our own address on the same /24 as `host`.

    Needed when the workstation is multi-homed — joined to the device AP *and* to a
    network that owns the default route. macOS then does not reliably send traffic
    for 192.168.4.x out the AP interface, so connects intermittently time out even
    though ping and a source-bound socket both work. Binding the source address
    pins it to the right interface.

    Override with DMXFIRE_SRC=192.168.4.3 if the guess is wrong.
    """
    forced = os.environ.get("DMXFIRE_SRC")
    if forced:
        return forced

    import re
    import subprocess

    ip = host.replace("http://", "").replace("https://", "").split("/")[0].split(":")[0]
    if not re.match(r"^\d+\.\d+\.\d+\.\d+$", ip):
        return None
    prefix = ip.rsplit(".", 1)[0] + "."
    try:
        out = subprocess.run(["ifconfig"], capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return None
    for m in re.finditer(r"inet (\d+\.\d+\.\d+\.\d+)", out):
        addr = m.group(1)
        if addr.startswith(prefix) and addr != ip:
            return addr
    return None


class _Resp:
    """Minimal response shim so call sites keep the requests-shaped API."""

    def __init__(self, r):
        self.status_code = r.status
        self.content = r.data
        self.headers = r.headers

    @property
    def text(self) -> str:
        return self.content.decode("utf-8", "replace")

    def json(self) -> Any:
        return json.loads(self.content)

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            raise requests.HTTPError(
                f"{self.status_code} for {self.status_code}", response=self
            )


class Client:
    """Talks to the device over urllib3 rather than requests.

    requests fails against this device on a multi-homed workstation — plain
    `requests.get` times out while a raw socket, an HTTPConnectionPool and a
    PoolManager all succeed from the same process at the same moment. urllib3 is
    what requests wraps anyway, so using it directly loses nothing and removes a
    layer that demonstrably misbehaves here.

    Source binding is applied when we can find our own address on the device's
    subnet: macOS scopes the route to the AP interface (`ifscope en0`), so an
    unbound socket can leave via the default-route interface and never arrive.
    """

    def __init__(self, host: str, timeout: float = 5.0) -> None:
        self.host = host.rstrip("/")
        self.timeout = timeout
        self.source_address = _local_addr_for(self.host)
        kw: dict[str, Any] = {"retries": False, "maxsize": 4}
        if self.source_address:
            kw["source_address"] = (self.source_address, 0)
        self._pool = urllib3.PoolManager(**kw)

    # ---- low-level ----

    def _request(self, method: str, path: str, *, fields=None, body=None,
                 headers=None, timeout: float | None = None) -> _Resp:
        r = self._pool.request(
            method, f"{self.host}{path}",
            fields=fields, body=body, headers=headers,
            timeout=urllib3.Timeout(total=timeout or self.timeout),
        )
        return _Resp(r)

    def _post(self, path: str, data: dict[str, Any] | None = None) -> _Resp:
        # encode_multipart=False -> form-urlencoded, which is what handleSet() parses
        r = self._pool.request(
            "POST", f"{self.host}{path}",
            fields={k: str(v) for k, v in (data or {}).items()},
            encode_multipart=False,
            timeout=urllib3.Timeout(total=self.timeout),
        )
        resp = _Resp(r)
        resp.raise_for_status()
        return resp

    def _get(self, path: str) -> _Resp:
        resp = self._request("GET", path)
        resp.raise_for_status()
        return resp

    def post_raw(self, path: str, body: bytes, timeout: float = 15.0) -> _Resp:
        """POST a raw body. Does NOT raise on 4xx/5xx."""
        return self._request("POST", path, body=body, timeout=timeout)

    def post_multipart(self, path: str, blob: bytes, filename: str = "fw.bin",
                       timeout: float = 20.0) -> _Resp:
        """POST a multipart file upload, the shape /api/update actually parses.

        A raw body leaves the upload callback unfired and the request stalls until
        the read timeout, which looks like a hang rather than a refusal.
        """
        r = self._pool.request(
            "POST", f"{self.host}{path}",
            fields={"firmware": (filename, blob, "application/octet-stream")},
            timeout=urllib3.Timeout(total=timeout),
        )
        return _Resp(r)

    # ---- state ----

    def get_state(self) -> dict[str, Any]:
        return self._get("/api/state").json()

    def fsm_state(self) -> str:
        return self.get_state()["fsm"]["state"]

    # ---- button control ----

    def press(self) -> None:
        self._post("/api/button/press")

    def release(self) -> None:
        self._post("/api/button/release")

    def reset(self) -> None:
        self._post("/api/button/reset")

    # ---- purge / empty accumulator (bypasses the FSM entirely) ----

    def purge_start(self) -> None:
        self._post("/api/purge/start")

    def purge_stop(self) -> None:
        self._post("/api/purge/stop")

    # ---- audio ----

    def audio_arm(self) -> None:
        self._post("/api/audio/arm")

    def audio_disarm(self) -> None:
        self._post("/api/audio/disarm")

    def audio(self) -> dict[str, Any]:
        """The audio block from /api/state."""
        return self.get_state()["audio"]

    def set_audio(self, **fields: Any) -> None:
        """Post any subset of the audio config.

        Field names match the form fields handleSet() reads — audShotMs,
        audMinGapMs, audDutyPct, audDutyWinMs, audMaxOpenMs, audLeadMs,
        audStaleMs, audBassOn, audBassOff, audBeatMin, audDropMin, audDropGapMs,
        audDropShotMs, audLightMode, audLightDepth.

        Every field is hasArg-guarded on the device, so a partial post leaves the
        rest untouched.
        """
        data: dict[str, Any] = {"target": "audio"}
        data.update(fields)
        self._post("/set", data=data)

    # ---- configuration (form-urlencoded /set) ----

    def set_confluence(self, *, connected: bool, fireLevel: int) -> None:
        data: dict[str, Any] = {"target": "confluence", "fireLevel": fireLevel}
        if connected:
            data["connected"] = "on"
        self._post("/set", data=data)

    def set_button(
        self,
        *,
        mode: int,
        fireDurationMs: int,
        cooldownMs: int,
        endCuePattern: int = 0,
        endCueMs: int = 1000,
        machineGunBurstMs: int | None = None,
    ) -> None:
        data: dict[str, Any] = {
            "target": "button",
            "mode": mode,
            "fireDurationMs": fireDurationMs,
            "cooldownMs": cooldownMs,
            "endCuePattern": endCuePattern,
            "endCueMs": endCueMs,
        }
        if machineGunBurstMs is not None:
            data["machineGunBurstMs"] = machineGunBurstMs
        self._post("/set", data=data)

    def set_fire_uplight(self, *, r: int, g: int, b: int, w: int = 0) -> None:
        """Set the global uplight colour held while any valve is open.

        Posts explicit byte fields rather than the "#rrggbb" colour input the web
        UI uses; handleSet() accepts either shape.
        """
        self._post(
            "/set",
            data={"target": "fireup", "fireUpR": r, "fireUpG": g, "fireUpB": b, "fireUpW": w},
        )

    def set_all_towers(
        self,
        *,
        theme: str,
        brightness: int,
        flameLevel: int,
        speed: int = 100,
    ) -> None:
        self._post(
            "/set",
            data={
                "target": "all",
                "theme": theme,
                "brightness": brightness,
                "speed": speed,
                "flameLevel": flameLevel,
            },
        )

    def set_tower(
        self,
        index: int,
        *,
        connected: bool,
        theme: str,
        brightness: int,
        flameLevel: int,
        speed: int = 100,
    ) -> None:
        data: dict[str, Any] = {
            "target": str(index),
            "theme": theme,
            "brightness": brightness,
            "speed": speed,
            "flameLevel": flameLevel,
        }
        if connected:
            data["connected"] = "on"
        self._post("/set", data=data)

    # ---- helpers ----

    def wait_for_state(self, expected: str, timeout: float = 2.0, poll: float = 0.05) -> dict[str, Any]:
        deadline = time.monotonic() + timeout
        last_state = None
        while time.monotonic() < deadline:
            s = self.get_state()
            last_state = s["fsm"]["state"]
            if last_state == expected:
                return s
            time.sleep(poll)
        raise DeviceTimeout(
            f"timeout waiting for fsm.state={expected!r} (last seen {last_state!r}) after {timeout}s"
        )

    def wait_until(self, predicate, timeout: float = 2.0, poll: float = 0.05) -> dict[str, Any]:
        """Poll /api/state until predicate(state) is true; return that state."""
        deadline = time.monotonic() + timeout
        s = None
        while time.monotonic() < deadline:
            s = self.get_state()
            if predicate(s):
                return s
            time.sleep(poll)
        raise DeviceTimeout(f"timeout waiting for predicate after {timeout}s; last state={s!r}")
