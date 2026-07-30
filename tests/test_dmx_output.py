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
