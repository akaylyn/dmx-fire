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
    """END_CUE runs for buttonConfig.endCueMs, which defaults to 1000 ms."""
    device.set_button(mode=0, fireDurationMs=200, cooldownMs=2000, endCueMs=1000)
    device.press()
    device.wait_for_state("END_CUE", timeout=1.0)
    t0 = time.monotonic()
    device.wait_for_state("COOLDOWN", timeout=2.0)
    elapsed = time.monotonic() - t0
    # Generous bounds because of polling jitter and HTTP latency.
    assert 0.5 <= elapsed <= 1.8, f"END_CUE duration {elapsed:.2f}s outside expected ~1s"


def test_end_cue_is_configurable(device):
    """A short endCueMs shortens the state; it is no longer hardcoded."""
    device.set_button(mode=0, fireDurationMs=200, cooldownMs=2000, endCueMs=200)
    device.press()
    device.wait_for_state("END_CUE", timeout=1.0)
    t0 = time.monotonic()
    device.wait_for_state("COOLDOWN", timeout=2.0)
    elapsed = time.monotonic() - t0
    assert elapsed <= 0.8, f"END_CUE with endCueMs=200 took {elapsed:.2f}s — not honouring config"


def test_end_cue_zero_skips_the_state(device):
    """endCueMs=0 goes FIRE_ACTIVE → COOLDOWN with no END_CUE at all.

    This is the change that unblocks rapid retrigger: the old hardcoded 1000 ms
    END_CUE put a hard floor of ~1.1 s on the shot cycle no matter what cooldown
    was set to.
    """
    device.set_button(mode=0, fireDurationMs=200, cooldownMs=500, endCueMs=0)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    seen = []
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        st = device.fsm_state()
        seen.append(st)
        if st == "COOLDOWN":
            break
        time.sleep(0.02)

    assert "COOLDOWN" in seen, f"never reached COOLDOWN, saw {seen}"
    assert "END_CUE" not in seen, f"END_CUE must be skipped when endCueMs=0, saw {seen}"


def test_rapid_retrigger_cycle_is_fast(device):
    """With end cue off and cooldown at 0, a full shot cycle is well under a second.

    Floor is set by the DMX bus, not the FSM: DMX_FRAME_INTERVAL_MS = 50 means a
    valve can only change once per frame, so ~100 ms is the shortest expressible
    shot. This asserts the FSM is no longer the bottleneck.
    """
    device.set_button(mode=0, fireDurationMs=50, cooldownMs=0, endCueMs=0)

    t0 = time.monotonic()
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.release()
    device.wait_for_state("IDLE", timeout=2.0)
    elapsed = time.monotonic() - t0

    assert elapsed < 1.0, (
        f"full shot cycle took {elapsed:.2f}s — expected well under 1s "
        f"(was ~1.1s before endCueMs existed)"
    )


def test_short_tap_still_opens_a_valve(device):
    """A FIRE_ACTIVE window shorter than one DMX frame must still reach the wire.

    The DMX block samples fsmState once per 50 ms frame, so without the
    fsmConsumeFirePending() latch a 10 ms tap could fall entirely between two
    frames and command no fire at all.
    """
    TOWER_FIRE_CH = [8, 23, 38, 53]
    device.reset()
    device.set_all_towers(theme="green", brightness=128)
    device.set_button(mode=1, fireDurationMs=10, cooldownMs=0, endCueMs=0)

    # Fire a burst of very short taps and watch for the valve byte on the wire.
    saw_open = False
    for _ in range(15):
        device.press()
        device.release()
        s = device.get_state()
        if any(s["dmx"]["ch"][c - 1] == 200 for c in TOWER_FIRE_CH):
            saw_open = True
            break
        time.sleep(0.05)

    assert saw_open, (
        "a sub-frame tap never opened a tower valve — the one-frame fire latch "
        "(fsmConsumeFirePending) is not working"
    )
