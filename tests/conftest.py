"""Pytest fixtures for the DMX-Fire host harness."""

from __future__ import annotations

import os

import pytest

from api import Client

DEFAULT_HOST = "http://192.168.4.1"


@pytest.fixture(scope="session")
def device() -> Client:
    host = os.environ.get("DMXFIRE_HOST", DEFAULT_HOST)
    client = Client(host)
    # Sanity check — fail fast with a clear message if the device isn't reachable.
    try:
        client.get_state()
    except Exception as e:
        pytest.exit(
            f"Cannot reach DMX-Fire device at {host}: {e}\n"
            f"Set DMXFIRE_HOST to override, and make sure the laptop is on the device's WiFi.",
            returncode=2,
        )
    return client


@pytest.fixture(autouse=True)
def baseline(device: Client):
    """Reset FSM and apply a known config before each test.

    Uses short fireDurationMs/cooldownMs so timing-sensitive tests run fast.
    """
    device.reset()
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCuePattern=0)
    device.set_confluence(connected=True, fireLevel=255)
    for i in range(4):
        device.set_tower(i, connected=True, palette="green", brightness=128, flameLevel=255)
    yield
    # Best-effort cleanup so a failing test doesn't leave the rig in FIRE_ACTIVE.
    try:
        device.release()
        device.reset()
    except Exception:
        pass
