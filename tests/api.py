"""HTTP client for the DMX-Fire ESP32. Thin wrapper around requests."""

from __future__ import annotations

import time
from typing import Any

import requests


class DeviceTimeout(AssertionError):
    pass


class Client:
    def __init__(self, host: str, timeout: float = 5.0) -> None:
        self.host = host.rstrip("/")
        self.timeout = timeout

    # ---- low-level ----

    def _post(self, path: str, data: dict[str, Any] | None = None) -> requests.Response:
        r = requests.post(f"{self.host}{path}", data=data, timeout=self.timeout)
        r.raise_for_status()
        return r

    def _get(self, path: str) -> requests.Response:
        r = requests.get(f"{self.host}{path}", timeout=self.timeout)
        r.raise_for_status()
        return r

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
