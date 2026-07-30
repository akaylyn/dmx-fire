# Spec: Confluence solenoid addressing (CH1)

## Context

The Confluence is the central art-piece propane solenoid — a single valve driven
through a **3-channel DMX decoder**, with the solenoid wired to the decoder's
**first output**. It sits first in the DMX chain.

The decoder was originally addressed **A004**, so its first output landed on
universe **CH4**, and `confluenceWrite()` wrote the fire level there. In the field
the decoder was re-addressed to **A001** — it now listens on CH1–3 and the
solenoid answers to **CH1**. The firmware follows the hardware: the level goes to
**CH1**.

This is an addressing change only. It does **not** touch the per-tower valves,
which remain on their own decoders' CH4 (universe CH8 / 23 / 38 / 53) — see
[spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md) and
[spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md).

---

## Channel map

Confluence occupies the bottom of the universe. Only CH1 reaches a fixture output
that is wired to anything.

| DMX CH | Decoder output | Function | Value written |
|---|---|---|---|
| **1** | 1 | **PROPANE SOLENOID** | `confluenceConfig.fireLevel` when firing, else 0 |
| 2 | 2 | Unwired output | 0 |
| 3 | 3 | Unwired output | 0 |
| 4 | — (outside the decoder's span) | Unclaimed | 0 |

Universe valve channels overall: **1** (Confluence) and **8 / 23 / 38 / 53**
(towers). Nothing else opens propane.

### Set the fixture to

**A001**, 3-channel mode, solenoid on output 1.

---

## Firmware

### `confluence.cpp` / `confluence.h`

`confluenceWrite(uint8_t level)` writes the four-byte Confluence block:

```cpp
dmxShadowWrite(level, 1);  // solenoid
dmxShadowWrite(0,     2);
dmxShadowWrite(0,     3);
dmxShadowWrite(0,     4);  // unclaimed — parked at 0
```

`ConfluenceConfig` is unchanged (`connected`, `fireLevel`); only the destination
channel moved. `fireLevel` remains 0–255, where 255 is fully open.

### Why CH2–4 are explicitly zeroed

CH2 and CH3 are real decoder outputs with nothing connected, and CH4 belongs to no
fixture at all (Tower 0's decoder starts at A005). None of them need a value — but
they are all written to 0 on every frame anyway, deliberately.

An unwritten channel keeps whatever byte the buffer last held. These three sit
immediately adjacent to a live valve channel on a bus that has exhibited noise and
phantom firing, so leaving them undriven buys nothing and costs a class of failure
that is very hard to diagnose in the field. The same reasoning drives the unclaimed
tails of each tower's stride to 0 — see
[spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md).

### Single write path

Every route that can open the central solenoid funnels through one
`confluenceWrite()` call in the 50 Hz DMX block of `Test_Button_DMX.ino`, so the
channel is defined in exactly one place. Priority within that block:

| Priority | Source | Level |
|---|---|---|
| 1 | **Purge** (Empty Accumulator) | `confluenceConfig.fireLevel` |
| 2 | **Morse** playback | `morseTick()` |
| 3 | **FSM `FIRE_ACTIVE`** | `fireLevel`, pulsed in machine-gun mode (mode 2) |
| — | anything else (IDLE / END_CUE / COOLDOWN) | 0 |

A disconnected Confluence (`connected == false`) skips the write entirely, exactly
as before.

---

## Rejected alternative: leave the decoder at A004

The device could have been left alone by re-addressing the decoder **back** to
A004 to match the already-flashed firmware — no reflash, a 30-second menu change.

This was rejected. A 3-channel decoder at A004 spans **CH4–6**, and CH5/CH6 are
Tower 0's accumulator strip **red and green** — live, animated theme data. The
decoder's outputs 2 and 3 would then be driven by tower animation. Nothing is
wired to those outputs today, so it would probably be harmless, but "probably
harmless animated data on a fixture that carries a propane valve" is not a
position worth defending, particularly during an active noise investigation.

At A001 the decoder's whole span is CH1–3, which the firmware drives explicitly
and which no other fixture touches.

---

## Web UI

The Confluence tab's `.dmx-addr` hint reads:

> Central solenoid driver: `A001`, 3-channel mode (CH 1 = central valve). Each
> tower's accumulator valve also opens via its own decoder CH 4 during fire.

Source of truth is `tools/web-preview/index.html`; mirrored into `web.cpp`'s
`buildPage()` F-strings. No control changes — **Connected** and **Fire level**
behave exactly as before.

---

## Persistence

None added. `connected` and `fireLevel` already persist in NVS. The channel is a
compile-time property of `confluenceWrite()`, not a setting — changing it requires
a firmware change, by design, so a config edit can never point a valve at the
wrong channel.

---

## Tests

`tests/test_dmx_output.py` asserts on `CONFLUENCE_FIRE_CH = 1`:

- `test_idle_no_fire` — CH1 is 0 in IDLE.
- `test_fire_active_drives_confluence` — CH1 equals `fireLevel` during `FIRE_ACTIVE`.
- `test_disconnected_confluence_stays_zero` — CH1 stays 0 when disconnected, even
  while the FSM is firing.
- `test_unclaimed_channels_stay_zero` — CH4 (among others) is held at 0.

The boot diagnostic (`tests.cpp` `testDmxVisual`) calls `confluenceWrite(0)` so the
central solenoid never opens during a power-on self test.

---

## Non-goals

- **Does not fix the DMX line-noise / phantom-firing problem.** That is an
  electrical fault in the transmitter path (weak RS-485 drive, idle biasing,
  ground reference) that scales with the number of connected towers, and it is
  unaffected by which channel the solenoid listens on. See
  [notes.md](../notes.md) for the field diagnosis and action plan.
- **No runtime-configurable solenoid channel.** There is deliberately no API or UI
  field for the DMX channel; a mis-set channel on a propane valve is a safety
  problem, so the mapping stays in firmware.
- **No change to the per-tower valves.** Tower fire stays on each decoder's CH4.
- **No reclaim of CH2–4 for other fixtures.** They stay parked at 0 rather than
  being handed to another device.
