"""Valve (propane solenoid) channel map, in one place.

Mirrors VALVE_CHANNELS in Test_Button_DMX/dmx.h, which is the firmware's own
declaration of the same fact. These five numbers used to be hand-copied into
test_dmx_output.py, test_audio.py, test_dmx_quiet.py, scripts/towers.sh and two
browser tools; the firmware now declares them, so the tests import one copy.

DMX is 1-indexed; /api/state's dmx.ch is 0-indexed (ch[0] = DMX channel 1).
Use ch() below rather than indexing directly.
"""

# Central solenoid — the Confluence decoder's first output (A001 CH1).
CONFLUENCE_FIRE_CH = 1

# Per-tower propane valve: the accumulator decoder's CH4, at 4 + i*15 + 4.
TOWER_FIRE_CH = [8, 23, 38, 53]

# Every channel that can open propane. Nothing else may.
VALVE_CHANNELS = [CONFLUENCE_FIRE_CH] + TOWER_FIRE_CH

# Uplight block start per tower; its 4 channels are R, G, B, W.
TOWER_UPLIGHT_CH = [9, 24, 39, 54]

# The only two bytes a valve channel may ever carry. A solenoid is an on/off
# device: values in between either fail to energise the coil or chatter it, so
# dmxShadowWrite() refuses them outright. See docs/spec-solenoid-binary.md.
VALVE_CLOSED = 0
VALVE_OPEN = 255
VALVE_BYTES = {VALVE_CLOSED, VALVE_OPEN}


def ch(state, dmx_ch):
    """One channel out of an /api/state snapshot, by 1-indexed DMX number."""
    return state["dmx"]["ch"][dmx_ch - 1]


def valve_bytes(state):
    """{channel: byte} for every valve channel in this snapshot."""
    return {c: ch(state, c) for c in VALVE_CHANNELS}


def assert_binary(state, where):
    """Every valve channel in this snapshot carries 0 or 255, nothing else."""
    bad = {c: v for c, v in valve_bytes(state).items() if v not in VALVE_BYTES}
    assert not bad, (
        f"{where}: valve channels must be 0 or 255, got "
        + ", ".join(f"CH{c}={v}" for c, v in sorted(bad.items()))
    )
