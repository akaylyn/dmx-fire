"""Per-tower configuration round-trip via /set target=<index>."""

import pytest


@pytest.mark.parametrize("idx", [0, 1, 2, 3])
def test_set_per_tower_palette_brightness(device, idx):
    device.set_tower(idx, connected=True, palette="blue", brightness=64, flameLevel=200)
    t = device.get_state()["towers"][idx]
    assert t["connected"] is True
    assert t["palette"] == "blue"
    assert t["brightness"] == 64
    assert t["flameLevel"] == 200


def test_per_tower_independent(device):
    device.set_tower(0, connected=True, palette="fire", brightness=10, flameLevel=20)
    device.set_tower(1, connected=True, palette="blue", brightness=200, flameLevel=100)
    towers = device.get_state()["towers"]
    assert towers[0]["palette"] == "fire"
    assert towers[0]["brightness"] == 10
    assert towers[1]["palette"] == "blue"
    assert towers[1]["brightness"] == 200
    # 2 and 3 still on baseline
    assert towers[2]["palette"] == "green"
    assert towers[3]["palette"] == "green"


def test_disconnect_tower(device):
    device.set_tower(2, connected=False, palette="green", brightness=128, flameLevel=255)
    assert device.get_state()["towers"][2]["connected"] is False


def test_palette_names(device):
    for name in ("green", "blue", "fire"):
        device.set_tower(0, connected=True, palette=name, brightness=128, flameLevel=255)
        assert device.get_state()["towers"][0]["palette"] == name
