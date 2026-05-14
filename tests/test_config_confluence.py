"""Confluence config round-trip."""


def test_confluence_fire_level(device):
    device.set_confluence(connected=True, fireLevel=128)
    c = device.get_state()["confluence"]
    assert c["connected"] is True
    assert c["fireLevel"] == 128


def test_confluence_disconnect(device):
    device.set_confluence(connected=False, fireLevel=255)
    c = device.get_state()["confluence"]
    assert c["connected"] is False
    assert c["fireLevel"] == 255


def test_confluence_zero_fire_level(device):
    device.set_confluence(connected=True, fireLevel=0)
    assert device.get_state()["confluence"]["fireLevel"] == 0
