"""Background UDP feature sender for the audio tests.

Wraps the canonical encoder from `tools/audio-sim/audio_packet.py` — imported,
never reimplemented, so the tests and the tuning CLI can never disagree about
the wire format (conftest.py puts that directory on sys.path).

The beat grid is *scheduled*, not emitted on demand: beat N happens at exactly
`start + N * 60/bpm`. That is what makes prediction testable — the test knows
when each acoustic beat was, so it can assert the valve opened BEFORE it.

    with AudioSender(host) as tx:
        tx.grid(bpm=120)
        time.sleep(3)
        edges = tx.beat_instants()      # monotonic timestamps to assert against
"""

from __future__ import annotations

import socket
import threading
import time
from typing import Any

import audio_packet as ap


class AudioSender:
    def __init__(self, host: str, port: int = ap.DEFAULT_PORT, rate: float = 40.0,
                 session: int = 4242, seq_start: int = 0) -> None:
        # Accept "http://192.168.4.1" as well as a bare IP, so tests can pass the
        # same host string the HTTP client uses.
        self.host = host.replace("http://", "").replace("https://", "").split("/")[0]
        self.port = port
        self.interval = 1.0 / rate
        self.session = session
        self._seq = seq_start

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Pin the source interface for the same reason the HTTP client does: on a
        # multi-homed workstation (device AP + a network holding the default route)
        # macOS does not reliably send 192.168.4.x traffic out the AP interface, and
        # UDP has no handshake to tell you it went nowhere.
        try:
            from api import _local_addr_for
            src = _local_addr_for(self.host)
            if src:
                self._sock.bind((src, 0))
                self.source_address = src
            else:
                self.source_address = None
        except Exception:
            self.source_address = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._lock = threading.Lock()

        # mutable state the test drives
        self._bands = {"bass": 0, "mid": 0, "treble": 0, "level": 0}
        self._bpm = 0
        self._grid_start: float | None = None
        self._beats_emitted = 0
        self._beat_instants: list[float] = []
        self._pending_beat = 0      # strength, 0 = none pending
        self._pending_hit = False
        self._silent = False
        self._loss = 0.0
        self._jitter_beats = False

    # ---- lifecycle ----

    def __enter__(self) -> "AudioSender":
        self.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2.0)
        self._sock.close()

    # ---- control ----

    def set_bands(self, *, bass: int | None = None, mid: int | None = None,
                  treble: int | None = None, level: int | None = None) -> None:
        with self._lock:
            for k, v in (("bass", bass), ("mid", mid), ("treble", treble), ("level", level)):
                if v is not None:
                    self._bands[k] = int(v)

    def grid(self, bpm: int, jitter: bool = False) -> None:
        """Start a scheduled beat grid. jitter=True makes intervals unstable, which
        should keep the device's predictor in its reactive fallback."""
        with self._lock:
            self._bpm = bpm
            self._grid_start = time.monotonic()
            self._beats_emitted = 0
            self._beat_instants = []
            self._jitter_beats = jitter

    def stop_grid(self) -> None:
        with self._lock:
            self._bpm = 0
            self._grid_start = None

    def beat(self, strength: int = 255) -> None:
        """Emit one beat on the next packet (no grid)."""
        with self._lock:
            self._pending_beat = max(1, int(strength))

    def bighit(self) -> None:
        with self._lock:
            self._pending_hit = True

    def go_silent(self, silent: bool = True) -> None:
        """Stop transmitting but keep the socket — for staleness tests."""
        with self._lock:
            self._silent = silent

    def set_loss(self, fraction: float) -> None:
        with self._lock:
            self._loss = fraction

    def beat_instants(self) -> list[float]:
        with self._lock:
            return list(self._beat_instants)

    # ---- worker ----

    def _run(self) -> None:
        import random

        next_send = time.monotonic()
        while not self._stop.is_set():
            now = time.monotonic()
            with self._lock:
                silent = self._silent
                bands = dict(self._bands)
                flags = 0
                strength = 0
                since_beat_ms = 0
                bpm_field = 0

                if self._pending_beat:
                    flags |= ap.FLAG_BEAT
                    strength = self._pending_beat
                    self._pending_beat = 0
                    self._beat_instants.append(now)

                if self._pending_hit:
                    flags |= ap.FLAG_BIGHIT
                    self._pending_hit = False

                if self._bpm and self._grid_start is not None:
                    period = 60.0 / self._bpm
                    elapsed = now - self._grid_start
                    due = int(elapsed / period)
                    if self._jitter_beats:
                        # Wobble hard enough that no four consecutive intervals look
                        # stable, so the device must fall back to reactive triggering.
                        due = int(elapsed / (period * (0.6 if (self._beats_emitted % 2) else 1.4)))
                    if due >= self._beats_emitted:
                        instant = self._grid_start + due * period
                        self._beats_emitted = due + 1
                        flags |= ap.FLAG_BEAT
                        strength = 255
                        since_beat_ms = min(int((now - instant) * 1000),
                                            ap.SINCE_BEAT_CLAMP_MS)
                        bpm_field = self._bpm
                        self._beat_instants.append(instant)
                    else:
                        bpm_field = self._bpm

                loss = self._loss
                seq = self._seq
                self._seq = (self._seq + 1) & 0xFFFF

            if not silent and (loss <= 0 or random.random() >= loss):
                pkt = ap.pack(
                    seq, self.session,
                    flags=flags,
                    beat_strength=strength,
                    bpm=bpm_field,
                    since_beat_ms=since_beat_ms,
                    frame_ms=25,
                    **bands,
                )
                try:
                    self._sock.sendto(pkt, (self.host, self.port))
                except OSError:
                    pass

            next_send += self.interval
            sleep = next_send - time.monotonic()
            if sleep > 0:
                self._stop.wait(sleep)
            else:
                next_send = time.monotonic()
