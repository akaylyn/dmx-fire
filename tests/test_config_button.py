"""Button FSM config round-trip via /set target=button."""

import pytest


@pytest.mark.parametrize("mode", [0, 1])
def test_button_mode(device, mode):
    device.set_button(mode=mode, fireDurationMs=1000, cooldownMs=2000)
    assert device.get_state()["button"]["mode"] == mode


def test_button_durations(device):
    device.set_button(mode=0, fireDurationMs=2500, cooldownMs=5000)
    b = device.get_state()["button"]
    assert b["fireDurationMs"] == 2500
    assert b["cooldownMs"] == 5000


def test_button_end_cue_pattern(device):
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCuePattern=0)
    assert device.get_state()["button"]["endCuePattern"] == 0
