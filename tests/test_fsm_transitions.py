"""Full FSM transition coverage: IDLE → FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE."""

import time


def test_idle_at_baseline(device):
    assert device.get_state()["fsm"]["state"] == "IDLE"


def test_press_enters_fire_active(device):
    device.press()
    s = device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    assert s["fsm"]["state"] == "FIRE_ACTIVE"


def test_full_cycle_fireball(device):
    """mode=0 (FIREBALL): full fireDurationMs then END_CUE → COOLDOWN → IDLE."""
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.release()  # FIREBALL ignores release; should still complete
    device.wait_for_state("END_CUE", timeout=1.5)
    device.wait_for_state("COOLDOWN", timeout=1.5)
    device.wait_for_state("IDLE", timeout=3.0)


def test_reset_forces_idle(device):
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.reset()
    # Reset is synchronous on the device — state should already be IDLE.
    s = device.get_state()
    assert s["fsm"]["state"] == "IDLE"


def test_end_cue_lasts_about_one_second(device):
    """END_CUE is hardcoded to 1000 ms in button_fsm.cpp."""
    device.set_button(mode=0, fireDurationMs=200, cooldownMs=2000)
    device.press()
    device.wait_for_state("END_CUE", timeout=1.0)
    t0 = time.monotonic()
    device.wait_for_state("COOLDOWN", timeout=2.0)
    elapsed = time.monotonic() - t0
    # Generous bounds because of polling jitter and HTTP latency.
    assert 0.5 <= elapsed <= 1.8, f"END_CUE duration {elapsed:.2f}s outside expected ~1s"
