"""Confluence config round-trip.

The old `fireLevel` slider is gone: CH1 drives an on/off solenoid, so the only
thing left to configure is whether it may open at all.
See docs/spec-solenoid-binary.md.
"""


def test_confluence_fire_enabled(device):
    device.set_confluence(connected=True, fireEnabled=True)
    c = device.get_state()["confluence"]
    assert c["connected"] is True
    assert c["fireEnabled"] is True


def test_confluence_fire_disabled(device):
    device.set_confluence(connected=True, fireEnabled=False)
    c = device.get_state()["confluence"]
    assert c["connected"] is True
    assert c["fireEnabled"] is False


def test_confluence_disconnect(device):
    device.set_confluence(connected=False, fireEnabled=True)
    c = device.get_state()["confluence"]
    assert c["connected"] is False
    assert c["fireEnabled"] is True


def test_connected_and_fire_enabled_are_independent(device):
    """The two flags mean different things and must not be conflated.

    `connected` blanks the whole fixture; `fireEnabled` isolates only its
    propane. That separation is the reason fireEnabled exists.
    """
    device.set_confluence(connected=False, fireEnabled=False)
    c = device.get_state()["confluence"]
    assert c == {"connected": False, "fireEnabled": False}


def test_no_fire_level_field(device):
    """There is no valve level to report, because there is none to set."""
    assert "fireLevel" not in device.get_state()["confluence"]
