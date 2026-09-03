"""/set target=all applies theme/brightness/speed to all towers at once.

Deliberately does NOT carry fireEnabled — see the note in test_apply_to_all_
does_not_change_fire_enabled below.
"""


def test_apply_to_all_towers(device):
    device.set_all_towers(theme="fire", brightness=42, speed=150)
    for t in device.get_state()["towers"]:
        assert t["theme"] == "fire"
        assert t["brightness"] == 42
        assert t["speed"] == 150


def test_apply_to_all_does_not_change_connected(device):
    """target=all should not touch the connected flag (matches existing semantics)."""
    device.set_tower(2, connected=False, theme="green", brightness=128)
    device.set_all_towers(theme="blue", brightness=64)
    towers = device.get_state()["towers"]
    assert towers[2]["connected"] is False
    assert towers[2]["theme"] == "blue"
    assert towers[2]["brightness"] == 64


def test_apply_to_all_does_not_change_fire_enabled(device):
    """Apply-to-All is a look control and must not reach the propane isolator.

    Both flags are checkboxes, and a browser submits nothing for an unchecked
    box. If target=all read fireEnabled, one "Apply to All" from a form that has
    no such box would clear it on all four towers at once — silently disabling
    every valve. So target=all leaves it alone entirely.
    """
    device.set_tower(1, connected=True, theme="green", brightness=128, fireEnabled=False)
    device.set_all_towers(theme="blue", brightness=64)
    towers = device.get_state()["towers"]
    assert towers[1]["fireEnabled"] is False, "target=all must not re-enable fire"
    assert towers[0]["fireEnabled"] is True, "target=all must not disable fire either"
    assert towers[1]["theme"] == "blue", "the look should still have been applied"
