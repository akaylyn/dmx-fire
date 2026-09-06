"""Per-tower configuration round-trip via /set target=<index>."""

import pytest

THEMES = ("green", "blue", "fire", "simon", "rainbow", "warm_white", "bright_white", "candle")


@pytest.mark.parametrize("idx", [0, 1, 2, 3])
def test_set_per_tower_theme_brightness(device, idx):
    device.set_tower(idx, connected=True, theme="blue", brightness=64, speed=200)
    t = device.get_state()["towers"][idx]
    assert t["connected"] is True
    assert t["fireEnabled"] is True
    assert t["theme"] == "blue"
    assert t["brightness"] == 64
    assert t["speed"] == 200


def test_per_tower_independent(device):
    device.set_tower(0, connected=True, theme="fire", brightness=10)
    device.set_tower(1, connected=True, theme="blue", brightness=200)
    towers = device.get_state()["towers"]
    assert towers[0]["theme"] == "fire"
    assert towers[0]["brightness"] == 10
    assert towers[1]["theme"] == "blue"
    assert towers[1]["brightness"] == 200
    # 2 and 3 still on baseline
    assert towers[2]["theme"] == "green"
    assert towers[3]["theme"] == "green"


def test_disconnect_tower(device):
    device.set_tower(2, connected=False, theme="green", brightness=128)
    assert device.get_state()["towers"][2]["connected"] is False


@pytest.mark.parametrize("idx", [0, 1, 2, 3])
def test_fire_enabled_is_per_tower(device, idx):
    """Isolating one tower's propane must not touch the other three."""
    device.set_tower(idx, connected=True, theme="green", brightness=128, fireEnabled=False)
    towers = device.get_state()["towers"]
    assert towers[idx]["fireEnabled"] is False
    for other in range(4):
        if other != idx:
            assert towers[other]["fireEnabled"] is True, (
                f"disabling tower {idx} also disabled tower {other}"
            )


def test_fire_enabled_survives_a_look_change(device):
    """Changing theme/brightness on a tower must not re-enable its propane."""
    device.set_tower(0, connected=True, theme="green", brightness=128, fireEnabled=False)
    device.set_tower(0, connected=True, theme="blue", brightness=64, fireEnabled=False)
    t = device.get_state()["towers"][0]
    assert t["fireEnabled"] is False
    assert t["theme"] == "blue"


def test_no_flame_level_field(device):
    """There is no valve level to report, because there is none to set."""
    for t in device.get_state()["towers"]:
        assert "flameLevel" not in t


def test_theme_names(device):
    for name in THEMES:
        device.set_tower(0, connected=True, theme=name, brightness=128)
        assert device.get_state()["towers"][0]["theme"] == name
