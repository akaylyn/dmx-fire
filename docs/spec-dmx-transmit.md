# Spec: DMX512 transmit path

## Context

Frames were originally emitted by `SparkFunDMX::update()`. During field debugging,
tower 1's accumulator decoder flickered continuously and its valve channel (CH23)
carried noise, while:

- the **same decoder driven by a manual DMX console was completely clean**;
- the **uplight on the same cable at the same point in the chain was stable**;
- the flicker **persisted with tower 1's channels set to 0**, meaning the decoder
  was not reading CH20–23 at all.

Those three together rule out the cable, the connector, termination, the decoder's
address (verified `020`, 4-channel) and the decoder itself. The only variable left
was the transmitter, which pointed at two defects in the library's `update()`.

### Defect 1 — break at the spec floor

`SparkFunDMX.h` defines `DMX_BREAK_DURATION_MICROS 88`, giving
`breakBaud = 1000000 * 8 / 88 = 90909`. The low pulse is the start bit plus 8 zero
data bits — 9 bit-times — so:

```
break = 9 / 90909 =  99 us     (spec floor 88 us — 11 us of margin)
MAB   = 2 / 90909 =  22 us     (spec floor  8 us)
```

99 µs is legal but marginal. Cheap decoders are unreliable at break detection near
the floor; one that misses a break fails to resync and begins sampling mid-frame,
so its channels latch whatever data sits at the slipped offset. A fixture whose
4th output is a propane valve then opens on animated colour data. Stage fixtures
with proper receivers (the uplights) tolerate 99 µs, which is exactly the split
observed. Consoles typically send 176 µs or more.

### Defect 2 — no flush after the frame

`SparkFunDMX.cpp` writes the frame and returns immediately, leaving ~2.9 ms of
transmission queued. The next call opens with `updateBaudRate(breakBaud)`; changing
the UART divider while bytes are in the shift register mangles the tail of the frame
in flight. `dmxKeepalive()` made this reachable — it called `update()` on its own
1 s schedule, and its `sending == 0` guard inspected only the TX buffer, not the
shift register.

---

## Technical details

Frames are emitted directly on `Serial1` by `dmx.cpp`. `SparkFunDMX` is no longer
used for transmit.

**Line format:** 250 kbaud, `SERIAL_8N2`, TX on GPIO 2.

**Frame sequence** (`dmxUpdate()`):

| Step | Action |
|------|--------|
| 0 | `if (!dmxReadyToSend()) return` — skip if the previous frame is still enqueued |
| 1 | `updateBaudRate(50000)` |
| 2 | `write(0)` → 180 µs break + 40 µs MAB |
| 3 | `flush()` — break must complete before the divider changes back |
| 4 | `updateBaudRate(250000)` |
| 5 | `write(0x00)` — DMX null start code |
| 6 | `write(dmxLastFrame, DMX_FRAME_SLOTS)` — channels 1–64 |
| 7 | `flush()` — frame fully out before returning |

`BREAK_BAUD = 50000` yields a **180 µs break and 40 µs MAB**, roughly double the
spec floor on both. Do not raise it back toward 90000 without re-testing every
decoder on the bus.

**TX-readiness query (step 0).** Before starting a break, `dmxUpdate()` calls
`dmxReadyToSend()`, which compares `Serial1.availableForWrite()` against the idle
free-space baseline captured in `dmxSetup()`. If a prior frame has not finished
draining, the tick is skipped rather than dropping a break onto bytes still in
flight. This is a non-blocking guard (the "query the UART before sending again"
requirement); the trailing `flush()` normally makes the next call ready, so on the
single-writer path it never actually skips.

The trailing `flush()` blocks for ~2.9 ms (65 bytes × 44 µs). At the 20 Hz frame
interval (`DMX_FRAME_INTERVAL_MS = 50`) that is ~6% duty and it makes a mid-frame
baud change structurally impossible.

**Rate is deliberately slow (20 Hz).** `DMX_FRAME_INTERVAL_MS = 50`. Field
experience is that an unhurried transmitter is more reliable on this bus than
pushing toward continuous refresh — fewer frames to collide with loop jitter, more
settle margin for the cheap decoders. The trade-off is a longer high-Z idle window
between frames; revisit only if idle-line noise reappears.

**Frame length restored to 64 slots.** `DMX_FRAME_SLOTS = 64`. A 128-slot padding
experiment (toward the console's 512) regressed the accumulator light and was
reverted; the constant stays so the experiment is one line to redo, but re-scope
before trusting any value other than 64. See notes.md "START HERE".

**Single writer.** `dmxKeepalive()` is deleted. The 20 Hz loop in
`Test_Button_DMX.ino` is the only caller of `dmxUpdate()` during normal operation
(`tests.cpp` also calls it during the boot diagnostic, before that loop starts). At
20 frames/sec no fixture times out, so the keepalive had no purpose.

**Buffer.** `dmxLastFrame[64]` is now the transmitted frame itself rather than a
mirror of a second buffer inside the library — `dmxShadowWrite()` writes only there.
`/api/state` continues to expose it, so test assertions are unchanged and are now
reading exactly the bytes on the wire.

---

## Web UI changes

None.

## Persistence

None. Frame timing constants are compile-time (`dmx.cpp`).

---

## Non-goals

- **Refresh rate is set slow on purpose, not tuned for latency.** 20 Hz via
  `DMX_FRAME_INTERVAL_MS = 50`, leaving ~47 ms of idle per interval. Slowing the bus
  is the chosen direction (see Technical details); shortening the idle window toward
  continuous is the opposite mitigation and is deliberately *not* pursued here
  (cause #4 in [../notes.md](../notes.md)).
- **Does not address idle-line biasing.** The transceiver's DE is not driven by
  firmware (no wire for it on the Unit DMX's 4-pin Grove); whether the line is held
  in a defined mark state between frames is a hardware property. See cause #2 in
  notes.md.
- **Does not address drive strength, ground offset, or termination.** Those remain
  the open electrical items, and the console comparison above suggests they are not
  the cause of this particular fault.
- **No DMX receive.** Transmit only; `setComDir(DMX_READ_DIR)` is gone with the
  library dependency.
