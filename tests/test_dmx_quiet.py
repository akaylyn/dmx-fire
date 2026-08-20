"""DMX transmitter quiet mode — handing the bus to another controller.

Stopping frames makes every fixture latch its last commanded value, so the guard
that refuses a non-idle rig is the safety-critical part of this feature, not a
convenience. See docs/spec-dmx-quiet-mode.md.
"""

import json
import time


def test_quiet_toggles_and_is_reported(device):
    assert device.get_state()["dmx"]["quiet"] is False

    r = device.dmx_quiet_start()
    assert r.status_code == 200
    assert json.loads(r.text)["quiet"] is True
    assert device.get_state()["dmx"]["quiet"] is True

    device.dmx_quiet_stop()
    assert device.get_state()["dmx"]["quiet"] is False


def test_every_valve_is_closed_on_the_wire_before_going_quiet(device):
    """The frame flushed before silence must have all five valves shut.

    Fixtures hold their last commanded value once frames stop, so a valve left
    open here would stay open with nothing running to close it.
    """
    device.dmx_quiet_start()
    try:
        ch = device.get_state()["dmx"]["ch"]
        for valve in (1, 8, 23, 38, 53):
            assert ch[valve - 1] == 0, f"CH{valve} was {ch[valve - 1]}, must be 0"
    finally:
        device.dmx_quiet_stop()


def test_refused_while_firing(device):
    device.press()
    try:
        r = device.dmx_quiet_start()
        assert r.status_code == 409
        body = json.loads(r.text)
        assert body["ok"] is False
        assert "IDLE" in body["error"]
        assert device.get_state()["dmx"]["quiet"] is False
    finally:
        device.release()
        device.reset()


def test_refused_while_purging(device):
    device.purge_start()
    try:
        r = device.dmx_quiet_start()
        assert r.status_code == 409
        assert "purge" in json.loads(r.text)["error"]
    finally:
        device.purge_stop()


def test_stop_is_idempotent(device):
    device.dmx_quiet_stop()
    device.dmx_quiet_stop()
    assert device.get_state()["dmx"]["quiet"] is False


def test_frames_resume_after_stop(device):
    """Channels must animate again once the transmitter is un-muted."""
    device.dmx_quiet_start()
    device.dmx_quiet_stop()
    # Tower 0's strips run a theme; sample until something is non-zero.
    for _ in range(20):
        ch = device.get_state()["dmx"]["ch"]
        if any(ch[4:7]):
            return
        time.sleep(0.25)
    raise AssertionError("no tower output seen after leaving quiet mode")
