"""Per-tower configuration round-trip via /set target=<index>."""

import pytest

THEMES = ("green", "blue", "fire", "simon", "rainbow", "warm_white", "bright_white", "candle")


@pytest.mark.parametrize("idx", [0, 1, 2, 3])
def test_set_per_tower_theme_brightness(device, idx):
    device.set_tower(idx, connected=True, theme="blue", brightness=64, speed=200, flameLevel=200)
    t = device.get_state()["towers"][idx]
    assert t["connected"] is True
    assert t["theme"] == "blue"
    assert t["brightness"] == 64
    assert t["speed"] == 200
    assert t["flameLevel"] == 200


def test_per_tower_independent(device):
    device.set_tower(0, connected=True, theme="fire", brightness=10, flameLevel=20)
    device.set_tower(1, connected=True, theme="blue", brightness=200, flameLevel=100)
    towers = device.get_state()["towers"]
    assert towers[0]["theme"] == "fire"
    assert towers[0]["brightness"] == 10
    assert towers[1]["theme"] == "blue"
    assert towers[1]["brightness"] == 200
    # 2 and 3 still on baseline
    assert towers[2]["theme"] == "green"
    assert towers[3]["theme"] == "green"


def test_disconnect_tower(device):
    device.set_tower(2, connected=False, theme="green", brightness=128, flameLevel=255)
    assert device.get_state()["towers"][2]["connected"] is False


def test_theme_names(device):
    for name in THEMES:
        device.set_tower(0, connected=True, theme=name, brightness=128, flameLevel=255)
        assert device.get_state()["towers"][0]["theme"] == name
