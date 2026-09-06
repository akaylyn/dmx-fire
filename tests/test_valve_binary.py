"""Propane solenoid channels carry 0 or 255 and nothing else.

The rig used to expose a per-tower `flameLevel` and a Confluence `fireLevel`,
each a 0-255 slider written straight to a valve channel. It never made a smaller
flame: the decoder has a turn-on threshold, so a mid-scale byte either left the
valve shut or chattered the coil. Flame size is gas pressure and orifice.

Those settings are gone. What replaces them is a boolean `fireEnabled` per
fixture, and a guard in dmxShadowWrite() that refuses any byte other than 0 or
255 on a valve channel. These tests assert the resulting contract from the
outside, over the same HTTP API an operator drives.

See docs/spec-solenoid-binary.md.
"""

import time

from valves import (
    CONFLUENCE_FIRE_CH,
    TOWER_FIRE_CH,
    TOWER_UPLIGHT_CH,
    VALVE_BYTES,
    VALVE_CHANNELS,
    assert_binary,
    ch,
    valve_bytes,
)

# One DMX frame is 50 ms (DMX_FRAME_INTERVAL_MS). Sample well inside a burn.
FRAME_S = 0.05


def _sample(device, n, gap=FRAME_S):
    """n consecutive /api/state snapshots, at least one DMX frame apart."""
    out = []
    for _ in range(n):
        out.append(device.get_state())
        time.sleep(gap)
    return out


def _observed(states, dmx_ch):
    """The set of bytes channel `dmx_ch` was seen carrying across `states`."""
    return {ch(s, dmx_ch) for s in states}


# --- the core invariant, under every source that can open a valve -----------


def test_valves_binary_during_fire(device):
    """A normal burn drives every valve to exactly 255, never part-way."""
    device.set_button(mode=0, fireDurationMs=3000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    states = _sample(device, 20)
    for i, s in enumerate(states):
        assert_binary(s, f"FIRE_ACTIVE sample {i}")

    # Not just "binary" — actually open. A test that only checked the value set
    # would pass on a rig whose valves never opened at all.
    for c in VALVE_CHANNELS:
        assert 255 in _observed(states, c), f"CH{c} never opened during FIRE_ACTIVE"

    device.release()


def test_valves_binary_during_purge(device):
    """Purge holds all five valves open, and holds them at 255."""
    device.purge_start()
    try:
        states = _sample(device, 20)
    finally:
        device.purge_stop()

    for i, s in enumerate(states):
        assert_binary(s, f"purge sample {i}")
    for c in VALVE_CHANNELS:
        assert _observed(states, c) == {255}, (
            f"CH{c} should be held fully open for the whole purge, saw {_observed(states, c)}"
        )


def test_valves_binary_during_machine_gun(device):
    """MACHINE_GUN chops the valve in time, not in amplitude.

    This is the mode most likely to regress into a ramp, so it asserts both
    halves: every sample is binary, AND both 0 and 255 are actually seen.
    """
    device.set_button(
        mode=2, fireDurationMs=4000, cooldownMs=2000, machineGunBurstMs=100
    )
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    # Sample faster than the burst period so both phases are caught.
    states = _sample(device, 60, gap=0.02)
    device.release()

    for i, s in enumerate(states):
        assert_binary(s, f"MACHINE_GUN sample {i}")

    for c in VALVE_CHANNELS:
        seen = _observed(states, c)
        assert seen == VALVE_BYTES, (
            f"CH{c} should pulse between exactly 0 and 255 in MACHINE_GUN, saw {sorted(seen)}"
        )


def test_valves_binary_during_morse(device):
    """Morse keys the central solenoid; each unit is fully on or fully off."""
    device.morse("SOS", unitMs=120)
    try:
        states = _sample(device, 40, gap=0.03)
    finally:
        device.morse_stop()

    for i, s in enumerate(states):
        assert_binary(s, f"morse sample {i}")

    seen = _observed(states, CONFLUENCE_FIRE_CH)
    assert seen <= VALVE_BYTES, f"morse drove CH1 to {sorted(seen)}"
    assert 255 in seen, "morse never opened CH1 — playback may not have started"


# --- the settings are really gone ------------------------------------------


def test_state_has_no_level_fields(device):
    """/api/state offers no valve level to read, because there is none to set."""
    s = device.get_state()

    assert "fireLevel" not in s["confluence"], (
        "confluence still reports a fireLevel — the solenoid has no level"
    )
    assert "fireEnabled" in s["confluence"], "confluence should report fireEnabled"

    for i, t in enumerate(s["towers"]):
        assert "flameLevel" not in t, f"tower {i} still reports a flameLevel"
        assert "fireEnabled" in t, f"tower {i} should report fireEnabled"


def test_legacy_flame_level_param_is_inert(device):
    """A stale client posting flameLevel= cannot set a partial valve byte.

    Old bookmarks and scripts still exist. The request must succeed — dropping
    it would leave an operator staring at an error mid-show — but the number
    must have no effect on the wire.
    """
    device._post(
        "/set",
        data={
            "target": "0",
            "connected": "on",
            "fireEnabled": "on",
            "theme": "green",
            "brightness": 128,
            "speed": 100,
            "flameLevel": 200,  # retired parameter
        },
    )

    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    device.release()

    assert ch(s, TOWER_FIRE_CH[0]) == 255, (
        f"legacy flameLevel=200 must not reach CH{TOWER_FIRE_CH[0]}, "
        f"got {ch(s, TOWER_FIRE_CH[0])}"
    )


# --- fireEnabled: the boolean that replaced the level ----------------------


def test_fire_enabled_round_trips(device):
    """Per-tower and Confluence fireEnabled persist and read back distinctly."""
    device.set_tower(0, connected=True, theme="green", brightness=128, fireEnabled=False)
    device.set_tower(1, connected=True, theme="green", brightness=128, fireEnabled=True)
    device.set_confluence(connected=True, fireEnabled=False)

    s = device.get_state()
    assert s["towers"][0]["fireEnabled"] is False
    assert s["towers"][1]["fireEnabled"] is True
    assert s["confluence"]["fireEnabled"] is False


def test_fire_enabled_false_keeps_valve_shut_but_lights_running(device):
    """Isolating one tower's propane must not take its lights with it.

    This is the whole reason fireEnabled exists rather than just reusing
    `connected`: unticking Connected blanks the fixture, which on the wire is
    indistinguishable from a dead decoder.
    """
    device.set_tower(0, connected=True, theme="green", brightness=128, fireEnabled=False)
    for i in (1, 2, 3):
        device.set_tower(i, connected=True, theme="green", brightness=128, fireEnabled=True)

    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    device.release()

    assert ch(s, TOWER_FIRE_CH[0]) == 0, (
        f"tower 0 has fire disabled; CH{TOWER_FIRE_CH[0]} must stay shut, "
        f"got {ch(s, TOWER_FIRE_CH[0])}"
    )
    for i in (1, 2, 3):
        assert ch(s, TOWER_FIRE_CH[i]) == 255, (
            f"tower {i} still has fire enabled; CH{TOWER_FIRE_CH[i]} should be open"
        )

    # The uplight still takes the fire look — an isolated tower is dark on
    # propane, not dark on stage.
    up = TOWER_UPLIGHT_CH[0]
    assert any(ch(s, up + n) > 0 for n in range(4)), (
        "tower 0's uplight went dark when its propane was isolated"
    )

    assert_binary(s, "fire with tower 0 isolated")


def test_confluence_fire_enabled_false(device):
    """The central solenoid can be isolated while the towers still fire."""
    device.set_confluence(connected=True, fireEnabled=False)

    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    device.release()

    assert ch(s, CONFLUENCE_FIRE_CH) == 0, (
        f"confluence fire is disabled; CH1 must stay shut, got {ch(s, CONFLUENCE_FIRE_CH)}"
    )
    for i in range(4):
        assert ch(s, TOWER_FIRE_CH[i]) == 255, f"tower {i} should still fire"


def test_purge_respects_fire_enabled(device):
    """Purge bypasses the FSM, but not the propane isolator."""
    device.set_tower(2, connected=True, theme="green", brightness=128, fireEnabled=False)

    device.purge_start()
    try:
        time.sleep(0.15)
        s = device.get_state()
    finally:
        device.purge_stop()

    assert ch(s, TOWER_FIRE_CH[2]) == 0, (
        f"purge must not open an isolated tower's valve, got {ch(s, TOWER_FIRE_CH[2])}"
    )
    assert ch(s, TOWER_FIRE_CH[0]) == 255, "other towers should still purge"
    assert_binary(s, "purge with tower 2 isolated")


# --- the latch hazard ------------------------------------------------------


def test_disconnected_tower_valve_is_forced_closed(device):
    """Disconnecting a tower mid-burn must not strand its valve open.

    A tower marked disconnected is skipped in the frame loop, and a DMX channel
    that stops being written keeps its last byte — so this used to leave the
    solenoid latched at 255 with nothing left to close it.
    """
    device.set_button(mode=0, fireDurationMs=4000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)

    assert ch(device.get_state(), TOWER_FIRE_CH[1]) == 255, "tower 1 should be firing"

    # Disconnect it while the burn is still running.
    device.set_tower(1, connected=False, theme="green", brightness=128, fireEnabled=True)
    time.sleep(0.15)
    s = device.get_state()
    device.release()

    assert ch(s, TOWER_FIRE_CH[1]) == 0, (
        f"disconnected tower 1 left CH{TOWER_FIRE_CH[1]} at {ch(s, TOWER_FIRE_CH[1])} "
        "— the valve is latched open"
    )


def test_disconnected_confluence_valve_is_forced_closed(device):
    """Same latch hazard on CH1."""
    device.set_button(mode=0, fireDurationMs=4000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)

    assert ch(device.get_state(), CONFLUENCE_FIRE_CH) == 255, "confluence should be firing"

    device.set_confluence(connected=False, fireEnabled=True)
    time.sleep(0.15)
    s = device.get_state()
    device.release()

    assert ch(s, CONFLUENCE_FIRE_CH) == 0, (
        f"disconnected confluence left CH1 at {ch(s, CONFLUENCE_FIRE_CH)} — valve latched open"
    )


# --- sweep -----------------------------------------------------------------


def test_valves_binary_across_a_full_fsm_cycle(device):
    """IDLE -> FIRE_ACTIVE -> END_CUE -> COOLDOWN -> IDLE, sampled throughout.

    The END_CUE white fade is the one place a ramp is deliberately generated.
    It belongs to the uplight's white channel; this proves none of it leaks
    onto a valve.
    """
    device.set_button(mode=0, fireDurationMs=400, cooldownMs=600, endCueMs=400)
    device.press()

    states = []
    deadline = time.monotonic() + 2.5
    while time.monotonic() < deadline:
        states.append(device.get_state())
        time.sleep(0.02)
    device.release()

    for i, s in enumerate(states):
        assert_binary(s, f"{s['fsm']['state']} sample {i}")

    seen_states = {s["fsm"]["state"] for s in states}
    assert "FIRE_ACTIVE" in seen_states, "never entered FIRE_ACTIVE"
    assert seen_states & {"END_CUE", "COOLDOWN"}, "never left FIRE_ACTIVE"


def test_no_other_channel_is_treated_as_a_valve(device):
    """Colour channels still dim normally — the guard must not overreach."""
    device.set_all_towers(theme="bright_white", brightness=200)
    time.sleep(0.15)
    s = device.get_state()

    # Strips are capped at 75%: 200 * 75 / 100 = 150. A non-binary byte on a
    # NON-valve channel is exactly what should still be possible.
    strips = [ch(s, c) for c in (5, 6, 7)]
    assert any(v not in (0, 255) for v in strips), (
        f"decoder strip channels should carry a mid-scale byte, got {strips}"
    )
    assert_binary(s, "idle with white theme")
