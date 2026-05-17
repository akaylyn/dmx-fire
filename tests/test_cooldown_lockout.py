"""Press during COOLDOWN is ignored (safety lockout)."""

import time


def test_press_during_cooldown_ignored(device):
    device.set_button(mode=0, fireDurationMs=300, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.release()
    device.wait_for_state("COOLDOWN", timeout=2.0)

    # While in COOLDOWN, a fresh press should NOT re-enter FIRE_ACTIVE.
    device.press()
    time.sleep(0.2)
    assert device.fsm_state() == "COOLDOWN", "press during COOLDOWN must be ignored"
    device.release()


def test_idle_after_cooldown(device):
    device.set_button(mode=0, fireDurationMs=200, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.release()
    # Walk through END_CUE → COOLDOWN → IDLE.
    device.wait_for_state("IDLE", timeout=5.0)
    # Now a press should fire again.
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
