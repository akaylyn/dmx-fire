#!/usr/bin/env python3
"""Fake the Atom EchoS3R audio node — stream feature packets to the controller.

This is what makes the whole audio feature testable and demoable with no Echo
hardware, no Echo firmware, and no FFT library. Point it at the device while
your workstation is on its AP:

    ./send_features.py --pattern music --bpm 128

Patterns
    beat     bare beat grid, minimal band energy   — beat pop / prediction
    bass     slow bass swells across the threshold — sustained bass, hysteresis
    drop     quiet bed with occasional BIGHIT      — drop-only mode
    sweep    bands sweep independently, no beats   — light modes, no fire
    silence  zeros with SILENCE set                — staleness / negative tests
    music    full 4/4 synthesis                    — the demo pattern

Negative / robustness flags
    --loss 0.2          drop 20% of packets, seq keeps advancing (gap counting)
    --stop-after 2.0    go silent after 2 s but hold the socket (staleness)
    --corrupt magic     also: version, short, long  (parser rejection)
    --flood 2000        ignore --rate, send this many packets/sec (drain bound)
    --session N         fix the session id (replay / session-reset tests)
    --seq-start N       start the sequence counter here

Run --selftest to round-trip the encoder without touching the network.

Stdlib only, deliberately: this has to run on a laptop at an event with no
package install. The wire format lives in audio_packet.py, which pytest imports
too, so the tool and the tests can never disagree.
"""

import argparse
import math
import random
import socket
import sys
import time

import audio_packet as ap

PATTERNS = ("beat", "bass", "drop", "sweep", "silence", "music")
CORRUPTIONS = ("magic", "version", "short", "long")


def _decay(t, tau):
    """Exponential decay envelope, 1.0 at t=0. t and tau in seconds."""
    if t < 0:
        return 0.0
    return math.exp(-t / tau) if tau > 0 else 0.0


class Synth:
    """Turns elapsed time into band energies plus beat/hit edges.

    The beat grid is *scheduled*, not random: beat N happens at exactly
    N * 60/bpm seconds. That is what lets the prediction tests assert the device
    fired ahead of a known instant rather than merely reacting to a packet.
    """

    def __init__(self, pattern, bpm):
        self.pattern = pattern
        self.bpm = bpm
        self.beat_period = 60.0 / bpm if bpm > 0 else 0.0
        self.beats_emitted = 0  # index of the next beat not yet reported

    def beat_index_at(self, t):
        if self.beat_period <= 0:
            return 0
        return int(t / self.beat_period)

    def beat_time(self, index):
        return index * self.beat_period

    def sample(self, t):
        """Return (bands, flags, beat_strength, since_beat_ms) for time t.

        `bands` is (bass, mid, treble, level), each 0-255.
        Beat edges are reported once, on the first sample at or after the beat
        instant — mirroring how the Echo would detect an onset.
        """
        flags = 0
        beat_strength = 0
        since_beat_ms = 0

        has_grid = self.pattern in ("beat", "music") and self.beat_period > 0

        if has_grid:
            # Report every beat whose instant has passed, but only the newest
            # one — at 40 Hz with any sane tempo at most one beat fits a frame.
            due = self.beat_index_at(t)
            if due >= self.beats_emitted:
                instant = self.beat_time(due)
                self.beats_emitted = due + 1
                flags |= ap.FLAG_BEAT
                # Downbeats hit harder, so beatMin thresholding is testable.
                beat_strength = 255 if due % 4 == 0 else 170
                since_beat_ms = min(
                    int((t - instant) * 1000.0), ap.SINCE_BEAT_CLAMP_MS
                )

        if self.pattern == "silence":
            return (0, 0, 0, 0), flags | ap.FLAG_SILENCE, 0, 0

        if self.pattern == "sweep":
            bass = int(127 + 127 * math.sin(2 * math.pi * t / 5.0))
            mid = int(127 + 127 * math.sin(2 * math.pi * t / 7.0))
            treble = int(127 + 127 * math.sin(2 * math.pi * t / 11.0))
            return (bass, mid, treble, max(bass, mid, treble)), flags, 0, 0

        if self.pattern == "bass":
            # Slow swell that crosses the default bassOn=170 / bassOff=140 band
            # in both directions, so hysteresis and chatter are observable.
            swell = 0.5 + 0.5 * math.sin(2 * math.pi * t / 6.0)
            bass = int(255 * swell)
            return (bass, bass // 3, bass // 6, bass), flags, 0, 0

        if self.pattern == "drop":
            # Quiet bed, with a big hit every 8 s. Ordinary beats are absent so
            # drop mode can be shown to ignore everything but the transient.
            hit_period = 8.0
            since_hit = t % hit_period
            if since_hit < 0.05 and t > 0.5:
                flags |= ap.FLAG_BIGHIT
            env = _decay(since_hit, 0.35)
            level = int(40 + 215 * env)
            return (int(30 + 225 * env), 30, 20, level), flags, 0, 0

        if self.pattern == "beat":
            # Bare grid: the beat edge and nothing else. Band energies stay well
            # below the default bassOn=170 so a beat-pop test cannot accidentally
            # satisfy sustained-bass mode, and no BIGHIT is ever emitted.
            beat_idx = self.beat_index_at(t)
            env = _decay(t - self.beat_time(beat_idx), 0.06)
            tick = int(80 * env)
            return (tick, tick // 2, tick // 2, tick), flags, beat_strength, since_beat_ms

        # --- music: kick on every beat, snare on 2 and 4, hats on 8ths ---
        beat_idx = self.beat_index_at(t)
        since_beat = t - self.beat_time(beat_idx)

        kick = _decay(since_beat, 0.09)
        bass = int(255 * kick)

        snare = 0.0
        if beat_idx % 2 == 1:  # beats 2 and 4 of a 4/4 bar
            snare = _decay(since_beat, 0.12)
        mid = int(60 + 195 * snare)

        half = self.beat_period / 2.0
        since_eighth = t % half if half > 0 else 0.0
        treble = int(200 * _decay(since_eighth, 0.04))

        level = max(bass, mid, treble)

        # A drop every 32 bars (128 beats), landing on the downbeat.
        if beat_idx % 128 == 0 and (flags & ap.FLAG_BEAT):
            flags |= ap.FLAG_BIGHIT

        return (bass, mid, treble, level), flags, beat_strength, since_beat_ms


def build_packet(seq, session, synth, t, bpm, corrupt=None):
    bands, flags, strength, since_beat_ms = synth.sample(t)
    bass, mid, treble, level = bands

    kwargs = dict(
        flags=flags,
        bass=bass,
        mid=mid,
        treble=treble,
        level=level,
        beat_strength=strength,
        bpm=bpm if synth.pattern in ("beat", "music") else 0,
        since_beat_ms=since_beat_ms,
        frame_ms=25,
    )
    if corrupt == "magic":
        kwargs["magic"] = b"XXXX"
    elif corrupt == "version":
        kwargs["version"] = 99

    data = ap.pack(seq, session, **kwargs)

    if corrupt == "short":
        data = data[:12]
    elif corrupt == "long":
        data = data + b"\x00" * 40
    return data, flags


def selftest():
    """Round-trip every pattern through the encoder. No network required."""
    print(f"packet size: {ap.SIZE} bytes  format: {ap.FORMAT}")
    failures = 0
    for pattern in PATTERNS:
        synth = Synth(pattern, 128)
        beats = hits = 0
        for i in range(400):  # 10 s at 40 Hz
            t = i / 40.0
            data, flags = build_packet(i, 4242, synth, t, 128)
            try:
                out = ap.unpack(data)
            except ValueError as exc:
                print(f"  FAIL {pattern}: {exc}")
                failures += 1
                break
            if out["seq"] != (i & 0xFFFF):
                print(f"  FAIL {pattern}: seq {out['seq']} != {i}")
                failures += 1
                break
            if out["since_beat_ms"] > ap.SINCE_BEAT_CLAMP_MS:
                print(f"  FAIL {pattern}: sinceBeatMs {out['since_beat_ms']} over clamp")
                failures += 1
                break
            beats += bool(flags & ap.FLAG_BEAT)
            hits += bool(flags & ap.FLAG_BIGHIT)
        else:
            expected = "~21 beats" if pattern in ("beat", "music") else "0 beats"
            print(f"  ok   {pattern:8s} beats={beats:3d} bighits={hits:2d}  ({expected})")

    for bad in CORRUPTIONS:
        synth = Synth("beat", 128)
        data, _ = build_packet(0, 1, synth, 0.0, 128, corrupt=bad)
        try:
            ap.unpack(data)
        except ValueError:
            print(f"  ok   corrupt={bad:8s} rejected as expected")
        else:
            print(f"  FAIL corrupt={bad} was accepted")
            failures += 1

    print("selftest FAILED" if failures else "selftest passed")
    return 1 if failures else 0


def main(argv=None):
    p = argparse.ArgumentParser(
        description="Stream fake audio feature packets to the dmx-fire controller.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Patterns: " + ", ".join(PATTERNS),
    )
    p.add_argument("--host", default="192.168.4.1", help="controller IP")
    p.add_argument("--port", type=int, default=ap.DEFAULT_PORT)
    p.add_argument("--pattern", choices=PATTERNS, default="music")
    p.add_argument("--bpm", type=int, default=128)
    p.add_argument("--rate", type=float, default=40.0, help="packets/sec")
    p.add_argument("--duration", type=float, default=0.0, help="0 = run forever")
    p.add_argument("--loss", type=float, default=0.0, help="drop fraction 0..1")
    p.add_argument("--stop-after", type=float, default=0.0,
                   help="go silent after N s but keep the socket open")
    p.add_argument("--corrupt", choices=CORRUPTIONS, default=None)
    p.add_argument("--flood", type=float, default=0.0,
                   help="override --rate with this packets/sec")
    p.add_argument("--session", type=int, default=None, help="fix the session id")
    p.add_argument("--seq-start", type=int, default=0)
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--selftest", action="store_true",
                   help="round-trip the encoder and exit; no network")
    args = p.parse_args(argv)

    if args.selftest:
        return selftest()

    if not 40 <= args.bpm <= 200 and args.pattern in ("beat", "music"):
        p.error("--bpm must be 40..200 for beat-grid patterns")
    if not 0.0 <= args.loss < 1.0:
        p.error("--loss must be in [0, 1)")

    rate = args.flood if args.flood > 0 else args.rate
    if rate <= 0:
        p.error("rate must be positive")
    interval = 1.0 / rate

    session = args.session if args.session is not None else random.randint(1, 65535)
    synth = Synth(args.pattern, args.bpm)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    if not args.quiet:
        extras = []
        if args.loss:
            extras.append(f"loss={args.loss:.0%}")
        if args.corrupt:
            extras.append(f"corrupt={args.corrupt}")
        if args.flood:
            extras.append("FLOOD")
        if args.stop_after:
            extras.append(f"silent after {args.stop_after}s")
        suffix = ("  [" + ", ".join(extras) + "]") if extras else ""
        print(
            f"-> {args.host}:{args.port}  pattern={args.pattern} "
            f"bpm={args.bpm} rate={rate:g}Hz session={session}{suffix}"
        )
        print("   ctrl-c to stop")

    seq = args.seq_start
    sent = dropped = beats = 0
    start = time.monotonic()
    next_send = start
    last_report = start

    try:
        while True:
            now = time.monotonic()
            t = now - start

            if args.duration and t >= args.duration:
                break

            silent = args.stop_after and t >= args.stop_after

            data, flags = build_packet(seq, session, synth, t, args.bpm, args.corrupt)
            if flags & ap.FLAG_BEAT:
                beats += 1

            if not silent:
                if args.loss and random.random() < args.loss:
                    dropped += 1  # seq still advances — that is the point
                else:
                    try:
                        sock.sendto(data, (args.host, args.port))
                        sent += 1
                    except OSError as exc:
                        print(f"send failed: {exc}", file=sys.stderr)
                        return 1
            seq = (seq + 1) & 0xFFFF

            if not args.quiet and now - last_report >= 2.0:
                state = "SILENT" if silent else f"{sent} sent"
                print(f"   t={t:6.1f}s  {state}  {dropped} dropped  {beats} beats")
                last_report = now

            next_send += interval
            sleep = next_send - time.monotonic()
            if sleep > 0:
                time.sleep(sleep)
            else:
                # Fell behind (flood mode, or a loaded machine). Resync rather
                # than spiralling into an ever-growing backlog.
                next_send = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    if not args.quiet:
        elapsed = time.monotonic() - start
        print(f"\n{sent} packets in {elapsed:.1f}s "
              f"({sent / elapsed if elapsed else 0:.0f}/s), "
              f"{dropped} dropped, {beats} beats")
    return 0


if __name__ == "__main__":
    sys.exit(main())
