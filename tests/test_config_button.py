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


def test_button_end_cue_ms_round_trip(device):
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCueMs=250)
    assert device.get_state()["button"]["endCueMs"] == 250
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCueMs=0)
    assert device.get_state()["button"]["endCueMs"] == 0


def test_button_fast_values_accepted(device):
    """The sliders now floor at 10 ms (0 for cooldown); the API must accept that."""
    device.set_button(mode=0, fireDurationMs=10, cooldownMs=0, endCueMs=0)
    b = device.get_state()["button"]
    assert (b["fireDurationMs"], b["cooldownMs"], b["endCueMs"]) == (10, 0, 0)


def test_fire_uplight_round_trip(device):
    device.set_fire_uplight(r=12, g=200, b=77, w=180)
    assert device.get_state()["fireUplight"] == {"r": 12, "g": 200, "b": 77, "w": 180}
