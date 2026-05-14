"""FIREBALL mode (mode=0): solenoid runs full fireDurationMs regardless of release."""

import time


def test_fireball_ignores_early_release(device):
    device.set_button(mode=0, fireDurationMs=1000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    # Release before fireDurationMs — should still be in FIRE_ACTIVE.
    time.sleep(0.2)
    device.release()
    time.sleep(0.2)
    assert device.fsm_state() == "FIRE_ACTIVE", "FIREBALL must ignore early release"

    # And complete naturally via timeout.
    device.wait_for_state("END_CUE", timeout=1.5)


def test_fireball_full_duration(device):
    device.set_button(mode=0, fireDurationMs=600, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    t0 = time.monotonic()
    device.wait_for_state("END_CUE", timeout=2.0)
    elapsed = time.monotonic() - t0
    # FIRE_ACTIVE should last ~fireDurationMs.
    assert 0.4 <= elapsed <= 1.2, f"FIRE_ACTIVE duration {elapsed:.2f}s outside expected ~0.6s"
