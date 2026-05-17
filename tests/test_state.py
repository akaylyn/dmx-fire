"""GET /api/state returns a sane shape with all expected sections."""


def test_state_shape(device):
    s = device.get_state()
    assert "uptime_ms" in s and isinstance(s["uptime_ms"], int)
    assert s["uptime_ms"] >= 0

    fsm = s["fsm"]
    assert fsm["state"] in {"IDLE", "FIRE_ACTIVE", "END_CUE", "COOLDOWN"}
    assert isinstance(fsm["elapsed_ms"], int)

    button = s["button"]
    for k in ("mode", "fireDurationMs", "cooldownMs", "endCuePattern"):
        assert k in button

    conf = s["confluence"]
    assert isinstance(conf["connected"], bool)
    assert 0 <= conf["fireLevel"] <= 255

    towers = s["towers"]
    assert len(towers) == 4
    for t in towers:
        assert isinstance(t["connected"], bool)
        assert t["palette"] in {"green", "blue", "fire"}
        assert 0 <= t["brightness"] <= 255
        assert 0 <= t["flameLevel"] <= 255

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
    assert s["confluence"]["connected"] is True
    assert s["confluence"]["fireLevel"] == 255
    for t in s["towers"]:
        assert t["connected"] is True
        assert t["palette"] == "green"
        assert t["brightness"] == 128
        assert t["flameLevel"] == 255
