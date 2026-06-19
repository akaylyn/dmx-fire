"""/set target=all applies theme/brightness/speed/flameLevel to all towers at once."""


def test_apply_to_all_towers(device):
    device.set_all_towers(theme="fire", brightness=42, speed=150, flameLevel=99)
    towers = device.get_state()["towers"]
    for t in towers:
        assert t["theme"] == "fire"
        assert t["brightness"] == 42
        assert t["speed"] == 150
        assert t["flameLevel"] == 99


def test_apply_to_all_does_not_change_connected(device):
    """target=all should not touch the connected flag (matches existing semantics)."""
    device.set_tower(2, connected=False, theme="green", brightness=128, flameLevel=255)
    device.set_all_towers(theme="blue", brightness=64, flameLevel=128)
    towers = device.get_state()["towers"]
    assert towers[2]["connected"] is False
    assert towers[2]["theme"] == "blue"
    assert towers[2]["brightness"] == 64
