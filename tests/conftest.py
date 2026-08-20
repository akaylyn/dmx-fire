"""Pytest fixtures for the DMX-Fire host harness."""

from __future__ import annotations

import os
import sys

import pytest

from api import Client

# The canonical packet encoder lives with the simulator, not in tests/. Importing it
# rather than copying it is deliberate: a second implementation would drift from the
# firmware struct silently, and the first symptom would be a test that passes while
# the device rejects every packet.
sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                    "tools", "audio-sim")
)

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


# Fixture config the rig is left in, applied both BEFORE and AFTER every test.
#
# The teardown half is load-bearing, not tidiness. test_storage.py deliberately writes
# connected=False to tower 1 and to the Confluence to prove distinct values round-trip,
# and it sorts last in the suite. Every config write lands in NVS (storage.cpp), so
# without this restore those two flags outlive the test run and survive reboots.
#
# A disconnected tower is silently skipped in the frame loop (Test_Button_DMX.ino:209
# — `if (!towerConfigs[i].connected) continue;`), so its channels are never written and
# sit at zero forever. On the wire that is indistinguishable from a dead decoder or a
# broken cable, and it cost a field session chasing exactly that: tower 1's accumulator
# was declared faulty and physically replaced before anyone checked the flag.
def _apply_baseline_config(device: Client) -> None:
    # Quiet mode is RAM-only so a reboot clears it, but a test that muted the
    # transmitter and died would leave the whole rig dark for the rest of the
    # session. Same failure shape as connected=false, so it gets the same restore.
    device.dmx_quiet_stop()
    device.set_confluence(connected=True, fireLevel=255)
    for i in range(4):
        device.set_tower(
            i, connected=True, theme="green", brightness=128, speed=100, flameLevel=255
        )


@pytest.fixture(autouse=True)
def baseline(device: Client):
    """Reset FSM and apply a known config before each test.

    Uses short fireDurationMs/cooldownMs so timing-sensitive tests run fast.
    """
    device.reset()
    # endCueMs is pinned to the firmware default so the timing tests below see the
    # same END_CUE they always did; tests that want rapid retrigger set it to 0.
    device.set_button(
        mode=0, fireDurationMs=500, cooldownMs=2000, endCuePattern=0, endCueMs=1000
    )
    device.set_fire_uplight(r=255, g=110, b=0, w=0)
    # Audio starts disarmed and with a short duty window, so budget tests converge in
    # seconds rather than tens of seconds. Everything else stays at firmware defaults.
    device.audio_disarm()
    device.set_audio(
        audDutyPct=25,
        audDutyWinMs=4000,
        audMinGapMs=250,
        audShotMs=150,
        audMaxOpenMs=600,
        audLeadMs=0,
        audStaleMs=500,
        audBassOn=170,
        audBassOff=140,
        audBeatMin=90,
        audDropGapMs=1000,
        audDropShotMs=300,
        audLightMode=1,
        audLightDepth=150,
    )
    _apply_baseline_config(device)
    yield
    # Best-effort cleanup so a failing test doesn't leave the rig in FIRE_ACTIVE —
    # or, just as bad, with a fixture marked disconnected. See _apply_baseline_config.
    try:
        device.audio_disarm()
        device.release()
        device.reset()
        _apply_baseline_config(device)
    except Exception:
        pass
