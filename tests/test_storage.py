"""Verify config writes are visible via /api/state.

A full power-cycle test for NVS persistence isn't possible from the host
harness — that's covered by the on-device testStorage() in tests.cpp.
This test confirms the firmware applies and reads back distinct values.
"""


def test_distinct_values_round_trip(device):
    device.set_tower(0, connected=True, palette="fire", brightness=11, flameLevel=22)
    device.set_tower(1, connected=False, palette="blue", brightness=33, flameLevel=44)
    device.set_tower(2, connected=True, palette="green", brightness=55, flameLevel=66)
    device.set_tower(3, connected=True, palette="blue", brightness=77, flameLevel=88)
    device.set_confluence(connected=False, fireLevel=99)
    device.set_button(mode=1, fireDurationMs=1500, cooldownMs=3000)

    s = device.get_state()
    assert s["towers"][0] == {"connected": True, "palette": "fire", "brightness": 11, "flameLevel": 22}
    assert s["towers"][1] == {"connected": False, "palette": "blue", "brightness": 33, "flameLevel": 44}
    assert s["towers"][2] == {"connected": True, "palette": "green", "brightness": 55, "flameLevel": 66}
    assert s["towers"][3] == {"connected": True, "palette": "blue", "brightness": 77, "flameLevel": 88}
    assert s["confluence"] == {"connected": False, "fireLevel": 99}
    assert s["button"]["mode"] == 1
    assert s["button"]["fireDurationMs"] == 1500
    assert s["button"]["cooldownMs"] == 3000
