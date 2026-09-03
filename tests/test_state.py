"""GET /api/state returns a sane shape with all expected sections."""


VALID_THEMES = {"green", "blue", "fire", "simon", "rainbow", "warm_white", "bright_white", "candle"}


def test_state_shape(device):
    s = device.get_state()
    assert "boot_id" in s and isinstance(s["boot_id"], str) and len(s["boot_id"]) > 0
    assert "uptime_ms" in s and isinstance(s["uptime_ms"], int)
    assert s["uptime_ms"] >= 0

    fsm = s["fsm"]
    assert fsm["state"] in {"IDLE", "FIRE_ACTIVE", "END_CUE", "COOLDOWN"}
    assert isinstance(fsm["elapsed_ms"], int)

    button = s["button"]
    for k in ("mode", "fireDurationMs", "cooldownMs", "endCuePattern", "endCueMs",
              "machineGunBurstMs"):
        assert k in button

    fire_up = s["fireUplight"]
    for k in ("r", "g", "b", "w"):
        assert 0 <= fire_up[k] <= 255

    conf = s["confluence"]
    assert isinstance(conf["connected"], bool)
    assert isinstance(conf["fireEnabled"], bool)
    assert "fireLevel" not in conf, "the solenoid has no level — see spec-solenoid-binary.md"

    towers = s["towers"]
    assert len(towers) == 4
    for t in towers:
        assert isinstance(t["connected"], bool)
        assert isinstance(t["fireEnabled"], bool)
        assert "flameLevel" not in t, "the valve has no level — see spec-solenoid-binary.md"
        assert t["theme"] in VALID_THEMES
        assert 0 <= t["brightness"] <= 255
        assert 10 <= t["speed"] <= 400

    dmx = s["dmx"]
    assert "ch" in dmx and len(dmx["ch"]) == 64
    for v in dmx["ch"]:
        assert 0 <= v <= 255


def test_baseline_applied(device):
    """conftest baseline should have applied known defaults."""
    s = device.get_state()
    assert s["fsm"]["state"] == "IDLE"
    assert s["button"]["mode"] == 0
    assert s["button"]["fireDurationMs"] == 500
    assert s["button"]["cooldownMs"] == 2000
    assert s["button"]["endCueMs"] == 1000
    assert s["fireUplight"] == {"r": 255, "g": 110, "b": 0, "w": 0}
    assert s["confluence"]["connected"] is True
    assert s["confluence"]["fireEnabled"] is True
    for t in s["towers"]:
        assert t["connected"] is True
        assert t["fireEnabled"] is True
        assert t["theme"] == "green"
        assert t["brightness"] == 128
        assert t["speed"] == 100
