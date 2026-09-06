"""Verify config writes are visible via /api/state.

A full power-cycle test for NVS persistence isn't possible from the host
harness — that's covered by the on-device testStorage() in tests.cpp.
This test confirms the firmware applies and reads back distinct values.
"""


def test_distinct_values_round_trip(device):
    device.set_tower(0, connected=True, theme="fire", brightness=11, speed=120, fireEnabled=True)
    device.set_tower(1, connected=False, theme="blue", brightness=33, speed=80, fireEnabled=False)
    device.set_tower(2, connected=True, theme="green", brightness=55, speed=200, fireEnabled=False)
    device.set_tower(3, connected=True, theme="blue", brightness=77, speed=300, fireEnabled=True)
    device.set_confluence(connected=False, fireEnabled=False)
    device.set_button(mode=1, fireDurationMs=1500, cooldownMs=3000)

    s = device.get_state()
    assert s["towers"][0] == {"connected": True, "fireEnabled": True, "theme": "fire", "brightness": 11, "speed": 120}
    assert s["towers"][1] == {"connected": False, "fireEnabled": False, "theme": "blue", "brightness": 33, "speed": 80}
    assert s["towers"][2] == {"connected": True, "fireEnabled": False, "theme": "green", "brightness": 55, "speed": 200}
    assert s["towers"][3] == {"connected": True, "fireEnabled": True, "theme": "blue", "brightness": 77, "speed": 300}
    assert s["confluence"] == {"connected": False, "fireEnabled": False}
    assert s["button"]["mode"] == 1
    assert s["button"]["fireDurationMs"] == 1500
    assert s["button"]["cooldownMs"] == 3000
