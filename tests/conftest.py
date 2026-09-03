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
# A disconnected tower is silently skipped in the frame loop
# (`if (!towerConfigs[i].connected)` in the .ino), so its lighting channels are never
# written and sit at zero forever. (Its VALVE is now explicitly closed on the way past
# — see docs/spec-solenoid-binary.md — but the lights still go dark.) On the wire that
# is indistinguishable from a dead decoder or a broken cable, and it cost a field session
# chasing exactly that: tower 1's accumulator was declared faulty and physically replaced
# before anyone checked the flag.
def _apply_baseline_config(device: Client) -> None:
    # Quiet mode is RAM-only so a reboot clears it, but a test that muted the
    # transmitter and died would leave the whole rig dark for the rest of the
    # session. Same failure shape as connected=false, so it gets the same restore.
    device.dmx_quiet_stop()
    # Morse bypasses the FSM and keys the central solenoid on its own clock, so a
    # test that started a message and died would keep tapping propane through the
    # rest of the run. Same restore reasoning as quiet mode above.
    device.morse_stop()
    # fireEnabled is restored for the same reason connected is: it is a checkbox,
    # so an omitted field reads as OFF, and a test that isolated one tower's
    # propane would otherwise leave it isolated in NVS across reboots.
    device.set_confluence(connected=True, fireEnabled=True)
    for i in range(4):
        device.set_tower(
            i, connected=True, theme="green", brightness=128, speed=100, fireEnabled=True
        )
    # Button config belongs in here too, not only in the fixture's setup half.
    #
    # It was omitted, and that gap bit a live rig on 2026-09-02: test_storage.py
    # writes mode=1 / 1500 / 3000 to prove distinct values round-trip and sorts
    # last, so its button config was the final NVS write and outlived the run —
    # silently replacing an operator's field tuning mid-session. Exactly the
    # Session 5 failure shape (a test's writes surviving as device config), just
    # on a different field. Anything a test can persist has to be restored here.
    device.set_button(
        mode=0, fireDurationMs=500, cooldownMs=2000, endCuePattern=0, endCueMs=1000
    )
    device.set_fire_uplight(r=255, g=110, b=0, w=0)


@pytest.fixture(autouse=True)
def baseline(device: Client):
    """Reset FSM and apply a known config before each test.

    Uses short fireDurationMs/cooldownMs so timing-sensitive tests run fast.
    """
    device.reset()
    # Button config and the fire-uplight colour now live in
    # _apply_baseline_config() so setup and TEARDOWN apply the identical set —
    # see the note there. endCueMs is pinned to the firmware default so the
    # timing tests see the same END_CUE they always did; tests that want rapid
    # retrigger set it to 0 themselves.
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
