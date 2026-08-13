"""DMX last-frame snapshot reflects config.

Universe layout (from towers.cpp / confluence.cpp):
  Confluence:  ch  1- 4  (ch 1 = solenoid; 3-ch decoder at A001)
  Tower 0:     ch  5-19  (decoder 5-8 fire=8, uplight 9-12, 13-19 unclaimed)
  Tower 1:     ch 20-34  (decoder 20-23 fire=23, uplight 24-27, 28-34 unclaimed)
  Tower 2:     ch 35-49  (decoder 35-38 fire=38, uplight 39-42, 43-49 unclaimed)
  Tower 3:     ch 50-64  (decoder 50-53 fire=53, uplight 54-57, 58-64 unclaimed)

Uplights run in 4-channel mode (R/G/B/W), so each tower claims 8 of its
15-channel stride and the rest is driven to 0.
"""

import time

# DMX is 1-indexed; dmx.ch is 0-indexed (dmx.ch[0] = DMX ch 1).
CONFLUENCE_FIRE_CH = 1

# Fire valve channel per tower (decoder CH4).
TOWER_FIRE_CH = [8, 23, 38, 53]

# Uplight block start per tower; the 4 channels are R, G, B, W.
TOWER_UPLIGHT_CH = [9, 24, 39, 54]

# Channels no fixture listens on — must be held at 0.
UNCLAIMED_CH = [4] + [c for base in (4, 19, 34, 49) for c in range(base + 9, base + 16)]


def ch(state, dmx_ch):
    return state["dmx"]["ch"][dmx_ch - 1]


def test_idle_no_fire(device):
    """In IDLE, confluence ch 1 must be 0 (no propane)."""
    # Give the DMX loop one tick to write IDLE values.
    time.sleep(0.1)
    s = device.get_state()
    assert ch(s, CONFLUENCE_FIRE_CH) == 0


def test_fire_active_drives_confluence(device):
    device.set_confluence(connected=True, fireLevel=200)
    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    # Wait one DMX frame (50 Hz = 20 ms).
    time.sleep(0.1)
    s = device.get_state()
    assert ch(s, CONFLUENCE_FIRE_CH) == 200, (
        f"confluence ch 1 should equal fireLevel during FIRE_ACTIVE, got {ch(s, CONFLUENCE_FIRE_CH)}"
    )


def test_fire_active_drives_tower_flame(device):
    for i in range(4):
        device.set_tower(i, connected=True, theme="green", brightness=128, flameLevel=180)
    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    for i, dmx_ch in enumerate(TOWER_FIRE_CH):
        assert ch(s, dmx_ch) == 180, (
            f"tower {i} fire valve (ch {dmx_ch}) should equal flameLevel=180 during FIRE_ACTIVE, got {ch(s, dmx_ch)}"
        )


def test_disconnected_confluence_stays_zero(device):
    device.set_confluence(connected=False, fireLevel=255)
    device.set_button(mode=0, fireDurationMs=1000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    assert ch(s, CONFLUENCE_FIRE_CH) == 0, (
        "disconnected confluence must not drive ch 1 even during FIRE_ACTIVE"
    )


def test_uplight_is_four_channel_rgbw(device):
    """Uplight block is R/G/B/W with no master dimmer or strobe gate.

    Regression guard for 11-channel mode: there, block CH1 was a master dimmer
    (written 255) and CH2 an RGB strobe gate (written 1). In 4-channel mode those
    same channels are Green and Blue, so a stale 11-ch write shows up as a bogus
    tint here. `bright_white` is used because it renders continuously (the fire
    gradients blank for 3200 ms of every 4000 ms cycle).
    """
    device.reset()
    device.set_all_towers(theme="bright_white", brightness=200, flameLevel=0)
    time.sleep(0.1)
    s = device.get_state()
    strip_expected = 200 * 75 // 100  # STRIP_BRIGHTNESS_PCT in towers.cpp
    for i, up in enumerate(TOWER_UPLIGHT_CH):
        r, g, b, w = (ch(s, up + n) for n in range(4))
        assert (r, g, b, w) == (200, 200, 200, 200), (
            f"tower {i} uplight (ch {up}-{up + 3}) should be R=G=B=W=200 uncapped, got {(r, g, b, w)}"
        )
        # Strips share the theme colour but are capped to protect the old supply.
        strip = TOWER_FIRE_CH[i] - 3
        assert ch(s, strip) == strip_expected, (
            f"tower {i} strip red (ch {strip}) should be capped to {strip_expected}, got {ch(s, strip)}"
        )


def test_unclaimed_channels_stay_zero(device):
    """No fixture listens on the stride tails — they must never drift nonzero.

    These sit next to valve channels, so a stale byte here is the failure mode
    worth guarding against.
    """
    device.reset()
    device.set_all_towers(theme="bright_white", brightness=255, flameLevel=255)
    time.sleep(0.1)
    s = device.get_state()
    nonzero = {c: ch(s, c) for c in UNCLAIMED_CH if ch(s, c) != 0}
    assert not nonzero, f"unclaimed channels must be 0, got {nonzero}"


def test_uplight_shows_fire_look_while_firing(device):
    """While a valve is open the uplight holds the configured fire colour.

    The default themes (green/blue/fire) blank for 3200 ms of every 4000 ms cycle,
    so before this the uplights were dark for most of a burn. The strips must be
    unaffected — they keep running the theme.
    """
    device.reset()
    device.set_fire_uplight(r=255, g=110, b=0, w=0)
    device.set_all_towers(theme="green", brightness=128, flameLevel=200)
    device.set_button(mode=0, fireDurationMs=3000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    # Sample across more than one full 4000 ms flash cycle: the uplight must read
    # the fire colour on EVERY sample, including the theme's OFF phase.
    seen = set()
    deadline = time.monotonic() + 2.5
    while time.monotonic() < deadline:
        s = device.get_state()
        if s["fsm"]["state"] != "FIRE_ACTIVE":
            break
        for up in TOWER_UPLIGHT_CH:
            seen.add(tuple(ch(s, up + n) for n in range(4)))
        time.sleep(0.1)

    assert seen == {(255, 110, 0, 0)}, (
        f"uplight must hold the fire colour for the whole burn, saw {sorted(seen)}"
    )


def test_fire_look_does_not_touch_strips(device):
    """The fire look is uplight-only; accumulator strips keep rendering the theme."""
    device.reset()
    device.set_fire_uplight(r=255, g=110, b=0, w=0)
    # bright_white renders continuously, so the strips have a stable expected value.
    device.set_all_towers(theme="bright_white", brightness=200, flameLevel=200)
    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()

    strip_expected = 200 * 75 // 100  # STRIP_BRIGHTNESS_PCT in towers.cpp
    for i, up in enumerate(TOWER_UPLIGHT_CH):
        assert tuple(ch(s, up + n) for n in range(4)) == (255, 110, 0, 0), (
            f"tower {i} uplight should show the fire look during FIRE_ACTIVE"
        )
        strip_r = TOWER_FIRE_CH[i] - 3
        assert ch(s, strip_r) == strip_expected, (
            f"tower {i} strip red (ch {strip_r}) must stay on the theme at {strip_expected}, "
            f"got {ch(s, strip_r)} — the fire look must not touch the strips"
        )


def test_uplight_returns_to_theme_after_fire(device):
    """Once the valve closes the uplight goes back to the theme."""
    device.reset()
    device.set_fire_uplight(r=255, g=110, b=0, w=0)
    device.set_all_towers(theme="bright_white", brightness=200, flameLevel=200)
    device.set_button(mode=0, fireDurationMs=300, cooldownMs=2000, endCueMs=0)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    device.release()
    device.wait_for_state("COOLDOWN", timeout=2.0)
    time.sleep(0.2)
    s = device.get_state()
    for i, up in enumerate(TOWER_UPLIGHT_CH):
        vals = tuple(ch(s, up + n) for n in range(4))
        assert vals == (200, 200, 200, 200), (
            f"tower {i} uplight should be back on the theme in COOLDOWN, got {vals}"
        )


def test_purge_lights_uplights_and_opens_all_valves(device):
    """Purge holds every valve open, so it must light the uplights too."""
    device.reset()
    device.set_fire_uplight(r=0, g=255, b=136, w=64)
    device.set_confluence(connected=True, fireLevel=255)
    device.set_all_towers(theme="green", brightness=128, flameLevel=200)
    device.purge_start()
    try:
        time.sleep(0.2)
        s = device.get_state()
        assert s["purge"] is True
        assert ch(s, CONFLUENCE_FIRE_CH) == 255
        for i, fire_ch in enumerate(TOWER_FIRE_CH):
            assert ch(s, fire_ch) == 200, f"tower {i} valve should be open during purge"
        for i, up in enumerate(TOWER_UPLIGHT_CH):
            vals = tuple(ch(s, up + n) for n in range(4))
            assert vals == (0, 255, 136, 64), (
                f"tower {i} uplight should show the fire look during purge, got {vals}"
            )
    finally:
        device.purge_stop()


def test_machine_gun_pulses_tower_valves(device):
    """MACHINE_GUN pulses the tower valves, not just the central solenoid.

    Previously the tower valves sat flat open for the whole burn while only
    Confluence pulsed. Both must now drop to 0 during a burst's off-phase.
    """
    device.reset()
    device.set_confluence(connected=True, fireLevel=255)
    device.set_all_towers(theme="green", brightness=128, flameLevel=200)
    device.set_button(
        mode=2, fireDurationMs=3000, cooldownMs=2000, endCueMs=0, machineGunBurstMs=100
    )
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)

    tower_vals, cf_vals = set(), set()
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        s = device.get_state()
        if s["fsm"]["state"] != "FIRE_ACTIVE":
            break
        tower_vals.add(ch(s, TOWER_FIRE_CH[0]))
        cf_vals.add(ch(s, CONFLUENCE_FIRE_CH))
        time.sleep(0.03)
    device.release()

    assert {0, 200} <= tower_vals, (
        f"tower valve should pulse between 0 and flameLevel in MACHINE_GUN, saw {sorted(tower_vals)}"
    )
    assert {0, 255} <= cf_vals, (
        f"confluence should pulse between 0 and fireLevel in MACHINE_GUN, saw {sorted(cf_vals)}"
    )


def test_disconnected_tower_skipped(device):
    """A disconnected tower should not have its channels written during FIRE_ACTIVE.

    The firmware skips disconnected towers (towerConfigs[i].connected == false) so
    their DMX channels retain whatever value was last written (zero from boot, or
    a previous run). After a reset to IDLE we expect them to read 0.
    """
    device.reset()
    # Tower 1 disconnected, others connected.
    device.set_tower(0, connected=True, theme="green", brightness=128, flameLevel=200)
    device.set_tower(1, connected=False, theme="green", brightness=128, flameLevel=200)
    device.set_tower(2, connected=True, theme="green", brightness=128, flameLevel=200)
    device.set_tower(3, connected=True, theme="green", brightness=128, flameLevel=200)
    device.set_button(mode=0, fireDurationMs=1500, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    # Connected towers should reflect flameLevel.
    assert ch(s, TOWER_FIRE_CH[0]) == 200
    assert ch(s, TOWER_FIRE_CH[2]) == 200
    assert ch(s, TOWER_FIRE_CH[3]) == 200
    # Disconnected tower 1 should NOT have been overwritten with flameLevel.
    assert ch(s, TOWER_FIRE_CH[1]) != 200
