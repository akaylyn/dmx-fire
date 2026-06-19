"""DMX last-frame snapshot reflects config.

Universe layout (from towers.cpp):
  Confluence:  ch 1-4   (only ch 4 = solenoid)
  Tower 0:     ch 5-19  (decoder 5-8, strobe 9-19)
  Tower 1:     ch 20-34
  Tower 2:     ch 35-49
  Tower 3:     ch 50-64

Each tower decoder CH4 (white) carries flameLevel; strobe CH11 mirrors it.
"""

import time

# DMX is 1-indexed; dmx.ch is 0-indexed (dmx.ch[0] = DMX ch 1).
CONFLUENCE_FIRE_CH = 4

# Decoder white channel per tower (the W / flame channel).
TOWER_DECODER_W_CH = [8, 23, 38, 53]


def ch(state, dmx_ch):
    return state["dmx"]["ch"][dmx_ch - 1]


def test_idle_no_fire(device):
    """In IDLE, confluence ch 4 must be 0 (no propane)."""
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
        f"confluence ch 4 should equal fireLevel during FIRE_ACTIVE, got {ch(s, CONFLUENCE_FIRE_CH)}"
    )


def test_fire_active_drives_tower_flame(device):
    for i in range(4):
        device.set_tower(i, connected=True, theme="green", brightness=128, flameLevel=180)
    device.set_button(mode=0, fireDurationMs=2000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    for i, dmx_ch in enumerate(TOWER_DECODER_W_CH):
        assert ch(s, dmx_ch) == 180, (
            f"tower {i} decoder W (ch {dmx_ch}) should equal flameLevel=180 during FIRE_ACTIVE, got {ch(s, dmx_ch)}"
        )


def test_disconnected_confluence_stays_zero(device):
    device.set_confluence(connected=False, fireLevel=255)
    device.set_button(mode=0, fireDurationMs=1000, cooldownMs=2000)
    device.press()
    device.wait_for_state("FIRE_ACTIVE", timeout=1.0)
    time.sleep(0.1)
    s = device.get_state()
    assert ch(s, CONFLUENCE_FIRE_CH) == 0, (
        "disconnected confluence must not drive ch 4 even during FIRE_ACTIVE"
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
    assert ch(s, TOWER_DECODER_W_CH[0]) == 200
    assert ch(s, TOWER_DECODER_W_CH[2]) == 200
    assert ch(s, TOWER_DECODER_W_CH[3]) == 200
    # Disconnected tower 1 should NOT have been overwritten with flameLevel.
    assert ch(s, TOWER_DECODER_W_CH[1]) != 200
