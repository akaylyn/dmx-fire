# Spec: Field DMX Diagnostics — Kit, Test App, and Verdict Method

## Context

The rig has a recurring class of field fault that is slow to localize under time
pressure: a tower flickers or a valve channel carries noise, and the operator has
to guess whether the problem is **the line** (a bad cable, connector, or
termination) or **the component** (a decoder, uplight, or its power). A full
bench-and-scope session already ran once and found — and replaced — a damaged
accumulator decoder, but that workflow does not fit a dusty field call.

This spec defines a **repeatable field procedure plus a purpose-built test app**
that turns "something is wrong on tower N" into a plain-English verdict —
**"LINE bad upstream"** or **"COMPONENT bad"** or **"fixture OK, controller/signal
path issue"** — in minutes, with a fixed kit and a laptop that is always on site.

**Root cause update (2026-08-04):** the recurring solenoid faults were ultimately
traced to a **missing DMX bus ground reference** — the galvanically-isolated M5 Unit
DMX leaves the bus unreferenced, so the decoders' RS-485 receivers drift out of
common-mode range and misread the valve channels. This is now the **first thing to
check** (Part 0 below); the termination test (Part 1) is demoted from top suspect.
Full write-up and sources: [dmx-isolated-grounding.md](dmx-isolated-grounding.md).

Related: [dmx-isolated-grounding.md](dmx-isolated-grounding.md) (the ground-reference
root cause + large-chain practice), [spec-dmx-transmit.md](spec-dmx-transmit.md) (the
ESP32 transmit path this diagnoses against), [hardware.md](hardware.md) (DMX channel
map), notes.md (field history — a lead, not authority).

**Safety:** propane stays disconnected / lines vented for all signal work. Valve
channels are exercised only to hear the solenoid click, never to fire gas.

---

## Part 0 — Ground reference (CONFIRMED root cause — check this first)

The isolated M5 Unit DMX gives the bus **no ground reference**, so the RS-485
common-mode drifts out of range and the decoders misread the valve channels
(solenoids don't fire, or chatter). Confirmed in the field: an **Enttec DMX USB Pro
on the bus — listening OR driving — fixes everything, including the ESP32's own
webapp commands**; remove it and it fails. A multimeter confirmed the M5's DMX
ground does not come out (it's isolated).

**Fast check:** with the rig failing, connect any **grounded, non-isolated DMX
device** (the Enttec, a grounded amp) to the bus. If the fixtures start responding,
the fault is the missing ground reference — **not** termination, drive, or firmware.

**Fix (reference from the fixture side — the M5 can't provide it):**
1. **Fail-safe bias resistors** referenced to the decoders' ground, or an **R‖C
   soft-ground** (we used 330 Ω ‖ 3 µF logic-gnd → bus-gnd on the bench).
2. **Grounded, powered opto-isolated splitter / re-driver** (Part 2) — the robust,
   field-portable answer; references from the properly-grounded fixture side.

Full explanation, large-chain rules, and sources:
[dmx-isolated-grounding.md](dmx-isolated-grounding.md).

---

## Part 1 — Termination test (secondary — no longer the top suspect)

> **Demoted (2026-08-04):** adding a device (the Enttec) *fixed* the bus, so it was
> under-referenced, not over-terminated. Do this only after Part 0. Kept because a
> genuine double-termination is still worth ruling out.

The M5Stack Unit DMX has a **120 Ω terminator on the module** (an onboard **switch**,
per [manuals/m5stack-unit-dmx.md](manuals/m5stack-unit-dmx.md)), and it sits at the
**source** (head) of the bus. A second 120 Ω at the last fixture therefore puts
**60 Ω** across the pair — halving the differential swing from the small isolated
driver. That would reproduce part of the field signature: position-dependent
dropouts, "more towers = more noise," and "the manual console copes but the ESP32
does not."

**Procedure:** power the bus down, meter across **XLR pins 2–3**.

| Reading | Meaning | Action |
|---------|---------|--------|
| ~120 Ω | Correctly single-terminated | OK |
| ~60 Ω  | **Double-terminated** | Remove one terminator (or the module's built-in, if switchable) |
| open / kΩ | No termination | Add one 120 Ω at the far fixture |

**Decision gate:** re-run the flickering tower off the ESP32 after correcting.
- Flicker **gone** → termination was the fault; daisy chain can stay.
- Flicker **persists** → drive strength / reflections remain → hub-and-spoke
  splitter (Part 2).

---

## Part 2 — Topology: hub-and-spoke splitter (contingent)

If the flicker survives the termination fix, insert an **opto-isolated DMX
splitter/booster** at the controller output and give **each tower its own branch**.
This re-drives a console-grade, actively-biased, independently-terminated signal per
branch and removes cumulative unit-load and shared reflections — the electrical
cause of the flicker — while making fault isolation trivial: a bad branch drops
alone, and per-port activity LEDs point at it. Optionally isolate the
valve-critical accumulator decoders on their own branch to reduce phantom-fire
exposure.

This is a **hardware/topology change**, not firmware — the ESP32 still emits one
universe; the splitter fans it out.

---

## Part 3 — Field kit (purchase list)

| Item | Role | Est. |
|------|------|------|
| DMX/XLR cable tester | Pin-by-pin cable + connector continuity; flags the bad-cable class instantly | ~$40 |
| Handheld DMX tester (RX + test-pattern TX) | Turnkey: read incoming channel values ("is signal here?") and inject a clean pattern ("does this fixture respond?") without a laptop | ~$130–180 |
| USB-DMX interface (Enttec DMX USB Pro or compatible clone, with RX) | Reference console + host for the test app (Part 4); RX path enables line-vs-component discrimination | ~$65 |
| Opto-isolated DMX splitter (4/8-way) — *contingent on Part 1* | Only if flicker survives the termination fix | ~$90–150 |

Core kit ≈ $235–285; the splitter is reserve budget. Target tier: **Mid ($150–500)**.

---

## Part 4 — Custom test app (`tools/dmx-tester/`)

A **self-contained browser app** (Web Serial, Chrome/Edge) that drives the USB-DMX
interface as a clean reference console. It exists because the operator wants
**one-click tests with pass/fail verdicts**, not a general lighting console (QLC+)
with a learning curve. Because it drives the interface — not the ESP32 — it is an
**independent reference source**, which is what makes the line-vs-component verdict
possible.

### Channel model (mirrors [hardware.md](hardware.md))

Per tower `base = 4 + index*15`; only 8 of each 15-slot stride are claimed.

| Tower | Decoder R/G/B/**FIRE** | Uplight R/G/B/W |
|-------|------------------------|-----------------|
| 0 | 5 / 6 / 7 / **8** | 9 / 10 / 11 / 12 |
| 1 | 20 / 21 / 22 / **23** | 24 / 25 / 26 / 27 |
| 2 | 35 / 36 / 37 / **38** | 39 / 40 / 41 / 42 |
| 3 | 50 / 51 / 52 / **53** | 54 / 55 / 56 / 57 |

Confluence solenoid = **CH1**. Valve channels: **1, 8, 23, 38, 53**.

### Tests (buttons)

- **Test Tower N** — steps that tower through R, G, B on the strips + uplight, then
  uplight white, then a single valve click (gas-off), pausing for the operator to
  confirm each visually/audibly.
- **Sweep valve channels** — pulse each valve channel in turn (1, 8, 23, 38, 53).
- **All fixtures white** — every uplight W up; quick "is anything totally dark?".
- **Solid R/G/B per fixture** — hold a colour to inspect one fixture.
- **Blackout / all-zero** — safe resting state; also the idle-noise reference.

### Verdict logic (plain English)

| Observation | Verdict |
|-------------|---------|
| App drives a fixture directly and it does **not** respond | **COMPONENT bad** (or its power) — the reference source is clean, so the fixture/decoder is at fault |
| Cable tester fails, or RX shows no valid frames past a point | **LINE bad upstream** — cable/connector/termination |
| Fixture responds to the app (clean console) but flickers off the ESP32 | **Fixture OK — controller/signal-path issue** (drive strength / termination — see Parts 1–2) |
| Idle valve channel shows nonzero / noise on RX while app sends 0 | **LINE noise reaching a valve channel** — electrical, keep gas off |

### RX monitor mode (interface permitting)

Tap a branch or the chain end into the interface's DMX **input** and display
received channel values, flagging any idle valve channel that is not a stable 0.
Note: a single interface generally cannot TX-at-head and RX-at-tail at the same
time — RX is a separate "monitor this point" mode, not live loopback.

### Build dependency

The interface model sets the serial protocol. **Default target: Enttec DMX USB Pro
packet protocol** (documented, Web-Serial-friendly, has RX). An FTDI "Open DMX"
type has no onboard framer and requires the app to bit-bang the break — feasible
but a different code path. **Confirm the purchased unit before building.**

### Style

Self-contained HTML/JS in the style of `tools/web-preview/` — no external deps,
runs from `file://`.

---

## Web UI changes

None to the firmware web UI. The test app is a separate standalone tool under
`tools/dmx-tester/`; it does not touch `Test_Button_DMX/web.cpp`.

## Persistence

None. The app is stateless per session; the kit and topology are physical.

---

## Non-goals

- **Not an on-device diagnostic.** This runs from a laptop against the USB-DMX
  interface, deliberately independent of the ESP32 transmitter under test. (The
  on-device `runDiagnostics()` / `testDmxVisual()` in `tests.cpp` remain the
  controller-side self-test.)
- **Not a full lighting console.** No cue stacks, no effects — QLC+ remains the
  fallback for anything beyond go/no-go testing.
- **No RDM**, no auto-addressing, no fixture discovery.
- **Does not commit to the splitter** unless the Part 1 termination test says so.
- **No firmware replacement.** The `Dmx_ESP32` library swap is tracked separately
  in the project plan, not here.
- **No live valve firing.** Valve channels are pulsed for an audible click only,
  gas disconnected.
