"""Canonical encoder/decoder for the dmx-fire audio feature packet.

This is the ONLY implementation of the wire format on the host side. Both
`send_features.py` and the pytest suite import it, so the format cannot drift
between the tool you tune with and the tests that guard the firmware.

The layout mirrors `struct AudioPacket` in `Test_Button_DMX/audio.h` exactly:

    off  size  field         type      units
    0    4     magic         char[4]   "DFAU"
    4    1     version       uint8     1
    5    1     flags         uint8     bitfield, see FLAG_* below
    6    2     session       uint16    random per Echo boot; resets seq tracking
    8    2     seq           uint16    +1 per packet, wraps
    10   1     bass          uint8     band energy 0-255, already AGC'd by the Echo
    11   1     mid           uint8
    12   1     treble        uint8
    13   1     level         uint8     broadband RMS
    14   1     beatStrength  uint8     0 unless FLAG_BEAT is set
    15   1     bpm           uint8     0 = unknown (40-200 fits a byte)
    16   2     sinceBeatMs   uint16    acoustic beat instant -> send time
    18   2     frameMs       uint16    analysis hop, nominal 25
    20   4     reserved      uint32    0

Little-endian on both ends and every field naturally aligned, so the firmware
memcpy's straight into the struct with no per-field swapping.

See docs/spec-audio-reactive.md for why there is no CRC and no HMAC.
"""

import struct

# '<' = little-endian, no padding. Keep this in lock-step with audio.h.
FORMAT = "<4sBBHHBBBBBBHHI"
SIZE = struct.calcsize(FORMAT)
assert SIZE == 24, f"wire format must be 24 bytes, got {SIZE}"

MAGIC = b"DFAU"
VERSION = 1
DEFAULT_PORT = 4210

# flags bitfield
FLAG_BEAT = 1 << 0  # a beat landed since the previous packet
FLAG_BIGHIT = 1 << 1  # large transient — a drop, a downbeat after a breakdown
FLAG_CLIP = 1 << 2  # input clipped; band values are unreliable
FLAG_SILENCE = 1 << 3  # no meaningful signal
FLAG_MICFAULT = 1 << 4  # mic read failed on the Echo

# The firmware clamps sinceBeatMs to this. Mirrored here so the simulator cannot
# accidentally generate values the device will silently reinterpret.
SINCE_BEAT_CLAMP_MS = 200


def _u8(v):
    return max(0, min(255, int(v)))


def _u16(v):
    return max(0, min(65535, int(v)))


def pack(
    seq,
    session,
    *,
    flags=0,
    bass=0,
    mid=0,
    treble=0,
    level=0,
    beat_strength=0,
    bpm=0,
    since_beat_ms=0,
    frame_ms=25,
    magic=MAGIC,
    version=VERSION,
    reserved=0,
):
    """Build one 24-byte packet. All numeric fields are clamped, not wrapped.

    `magic` and `version` are parameters only so the negative tests can corrupt
    them; normal callers leave them alone.
    """
    return struct.pack(
        FORMAT,
        magic,
        _u8(version),
        _u8(flags),
        _u16(session),
        _u16(seq) & 0xFFFF,
        _u8(bass),
        _u8(mid),
        _u8(treble),
        _u8(level),
        _u8(beat_strength),
        _u8(bpm),
        _u16(since_beat_ms),
        _u16(frame_ms),
        int(reserved) & 0xFFFFFFFF,
    )


def unpack(data):
    """Decode a packet into a dict. Raises ValueError on anything malformed.

    Used by the tests to assert on what was actually put on the wire, and by
    `send_features.py --selftest`.
    """
    if len(data) != SIZE:
        raise ValueError(f"expected {SIZE} bytes, got {len(data)}")
    fields = struct.unpack(FORMAT, data)
    out = {
        "magic": fields[0],
        "version": fields[1],
        "flags": fields[2],
        "session": fields[3],
        "seq": fields[4],
        "bass": fields[5],
        "mid": fields[6],
        "treble": fields[7],
        "level": fields[8],
        "beat_strength": fields[9],
        "bpm": fields[10],
        "since_beat_ms": fields[11],
        "frame_ms": fields[12],
        "reserved": fields[13],
    }
    if out["magic"] != MAGIC:
        raise ValueError(f"bad magic {out['magic']!r}")
    if out["version"] != VERSION:
        raise ValueError(f"bad version {out['version']}")
    return out


def flag_names(flags):
    """Render a flags byte as a readable list, for logging."""
    names = [
        (FLAG_BEAT, "BEAT"),
        (FLAG_BIGHIT, "BIGHIT"),
        (FLAG_CLIP, "CLIP"),
        (FLAG_SILENCE, "SILENCE"),
        (FLAG_MICFAULT, "MICFAULT"),
    ]
    return [name for bit, name in names if flags & bit] or ["-"]
