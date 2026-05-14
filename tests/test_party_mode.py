"""PARTY mode (mode=1): release ends fire early."""

import time


def test_party_release_ends_fire_early(device):
    device.set_button(mode=1, fireDurationMs=5000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.2)
    device.release()
    # Should leave FIRE_ACTIVE quickly, well before fireDurationMs (5s).
    device.wait_for_state("END_CUE", timeout=1.0)


def test_party_held_runs_to_timeout(device):
    """If you don't release in PARTY mode, fireDurationMs still applies."""
    device.set_button(mode=1, fireDurationMs=500, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    # Don't release — should time out via fireDurationMs.
    device.wait_for_state("END_CUE", timeout=1.5)
