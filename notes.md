# Field Debugging Notes — DMX Noise / Spurious Solenoid Firing

_Session dates: 2026-07-17 → 2026-07-20, continued 2026-07-29. Captured from the live field-debug conversations._

---

## ⚠ START HERE — device state as of 2026-07-29

**The last flash left the rig regressed: the accumulator light is not working.** That
appeared immediately after the final flash, which changed **two things at once** —
frame length 64 → 128 slots, and refresh 50 Hz → 20 Hz. Suspect one of those before
anything else.

### What is on the device right now

| Setting | Value | Where |
|---|---|---|
| Confluence solenoid | **CH1** | `confluence.cpp` |
| Uplight mode | 4-channel R/G/B/W | `towers.cpp` |
| Tower fire valves | decoder CH4 → ch **8 / 23 / 38 / 53** | `towers.cpp` |
| Break / MAB | **180 µs / 40 µs** | `dmx.cpp` — `BREAK_BAUD = 50000` |
| Frame slots | **128** (ch 65–128 zero padding) | `dmx.h` — `DMX_FRAME_SLOTS` |
| Refresh | **20 Hz** (50 ms) | `dmx.h` — `DMX_FRAME_INTERVAL_MS` |
| Addressed channels | 64 | `dmx.h` — `DMX_SHADOW_SIZE` |
| DMX writer | single (frame loop only) | `dmxKeepalive()` deleted |

### Fastest way back to a known-good baseline

Both are one-line constants in [Test_Button_DMX/dmx.h](Test_Button_DMX/dmx.h):

```cpp
static const uint16_t DMX_FRAME_SLOTS       = 64;   // was 128
static const uint8_t  DMX_FRAME_INTERVAL_MS = 20;   // was 50
```

Reflash and confirm the accumulator light returns. Then change **one at a time**, so
the next result is attributable to something.

### Leading hypothesis to test first — likely double termination

The transceiver is the **M5Stack Unit DMX**, which has a **120 Ω terminator built in**
([docs/manuals/m5stack-unit-dmx.md](docs/manuals/m5stack-unit-dmx.md) line 56). If
there is *also* a terminator at the last fixture, the bus is terminated **twice — 60 Ω**,
which halves the load impedance and cuts the differential swing from a small driver.

That single fact would explain the entire history in this file:

- position-dependent failures (reduced swing → receivers nearest a null drop out first)
- the manual console working (stronger driver copes with 60 Ω)
- "more towers connected = more noise" (more unit loads on an already-overloaded bus)

**Measure it: power the bus down, meter across XLR pins 2 and 3.** ~120 Ω = correct.
**~60 Ω = two terminators, remove one.** Open/kΩ = none. This has been flagged three
times and never measured; it is free and it is the top suspect.

---

## The situation

Project set up in the field.

- With **one tower connected** (Tower 0) + **Confluence connected**, everything is fine.
- Towers 1, 2, 3 are **not working**.
- **The more towers connected, the more noise on the line.**
- **Tower 0's solenoids fire a lot — more than the software allows** → strongly suggests the decoder is reading garbage (noise) on its fire channel, not a firmware command.

### Critical diagnostic clue (found later)
- **When using the manual (non-software) DMX controller, the problem goes away** — same wiring, same termination.
- **Plugging the 3rd tower back in makes it resume.**
- They believe the bus is **terminated correctly**.

→ Physical layer (cable, termination, decoders, grounding of the bus itself) is largely proven good by the manual console working. The differentiator is the **ESP32's DMX transmitter**, and it **degrades with load** (each tower = another RS-485 unit load).

> **This conclusion is now in doubt — see the connector finding below.** An
> intermittent connection produces the same "works sometimes / worse with more
> towers / changes when you disturb cables" signature as a weak transmitter, and it
> is far cheaper to fix. Rule the connectors out before buying a booster.

---

## Connector finding (Tower 0 uplight) — likely the real fault

**Symptom set, all explained by one bad connection:**
- All four uplights running a yellow/white-runner pattern → that is the LL960S
  **built-in demo program, which it runs when it receives no valid DMX at all**. Not
  a mode or address problem.
- Uplights not responding to the **manual** DMX console either → the signal was not
  reaching them; nothing was wrong with their config.
- **Tower 0's solenoid fired while towers 1/2/3 did not** → the Tower 0 decoder sits
  *upstream* of the fault; everything downstream received no data.

**Fix that worked:** unplugged the DMX cable from the Tower 0 uplight and plugged it
back in. Control was restored. **No fixture settings were changed.**

→ The fault is a **marginal/intermittent connection at that XLR**, not a failed
fixture and not the ESP32 transmitter. Reseating restored contact. Candidates:
oxidised or dirty contacts, a connector not fully latched, a bent/splayed pin, or a
failing solder joint in the connector shell.

> **A reseat is not a fix.** It will come back with vibration, heat, or anyone moving
> a cable — and an intermittent connector is exactly what makes a fault look
> load-dependent. Treat this as located, not solved.

**To close it out:**
- [ ] Replace that cable (or at least re-terminate that connector); test the removed
      one for continuity on **all three pins** while flexing it.
- [ ] **Wiggle test the whole chain** — with the rig running, gently flex each XLR in
      turn and watch for dropouts. This is what finds the remaining marginal ones.
- [ ] Confirm every XLR actually latches (audible click), not just friction-fits.
- [ ] Retest towers 1/2/3 solenoids. If they now fire, this single connector
      accounts for the whole "towers 1, 2, 3 are not working" symptom and the
      transmitter-degradation theory below can be deprioritised.

---

## Config changes made this session

Both changes below are **verified against the physical hardware and flashed to the device this session.**

### 1. Confluence solenoid moved from CH4 → CH1

The physical Confluence decoder was re-addressed to listen on channels 1/2/3 (was 4/5/6), so the firmware now fires the solenoid on **CH1**. It is a 3-channel RGB decoder at **A001** with the solenoid wired to its **first output**.

- [confluence.cpp:22-27](Test_Button_DMX/confluence.cpp#L22-L27) — `confluenceWrite()` writes `level` to CH1, `0` to CH2–4.
- CH4 is now unclaimed (Tower 0 starts at A005). It is still driven to 0 every frame rather than left undriven — an undriven channel sitting next to a valve channel is not worth the risk on this bus.
- Comments updated in [confluence.h](Test_Button_DMX/confluence.h), [morse.h:5](Test_Button_DMX/morse.h#L5), [tests.cpp:68](Test_Button_DMX/tests.cpp#L68).

Verified **all** solenoid output paths flow through `confluenceWrite()` — normal fire, purge (Empty Accumulator), and morse all route through [Test_Button_DMX.ino:128](Test_Button_DMX/Test_Button_DMX.ino#L128). No direct CH4 writes elsewhere. One change covers every path.

### 2. Uplights moved from 11-channel → 4-channel mode

**Found in the field: the uplights were in mixed modes — some 4-channel, some 8-channel** — while the firmware was sending 11-channel data to all of them. This is a candidate cause for the inconsistent per-tower behaviour recorded in **The situation** above ("towers 1, 2, 3 are not working").

Why mixed modes break things: in **8-channel** mode the LaluceNatz CH1 is a **master dimmer**, so a fixture left in 8-ch reads what the controller intends as a colour value as an overall brightness — and a 0 there blacks the fixture out entirely regardless of every other channel. A tower in 8-ch mode therefore behaves nothing like its 4-ch neighbour even when both are addressed correctly and both sit on a healthy bus.

All four uplights are now set to **4-channel mode** (R/G/B/W linear dimming) and the firmware writes 4 channels to match:

- [towers.cpp:55-63](Test_Button_DMX/towers.cpp#L55-L63) — uplight block is now `base+5 … base+8` = R/G/B/W at full brightness.
- [towers.cpp:65-69](Test_Button_DMX/towers.cpp#L65-L69) — `base+9 … base+15` are unclaimed and driven to 0 every frame.
- [towers.h:12-16](Test_Button_DMX/towers.h#L12-L16) — `TowerState` lost `masterDim` / `rgbStrobe` / `wStrobe`; 4-ch mode has no master dimmer and no strobe gate.
- [themes.cpp:78-81](Test_Button_DMX/themes.cpp#L78-L81) — brightness is baked straight into `r`/`g`/`b`/`white`.

> **Latent bug this avoided:** the old `rgbStrobe = 1` write landed on block CH2, which in 4-channel mode is **Green**. Had that write survived the mode switch, every tower would have carried a permanent faint green tint.

**Fixture start addresses did not change** — decoders stay at A005/A020/A035/A050, uplights at A009/A024/A039/A054. Only the fixture *mode* changed, so nothing needed re-addressing in the field. The 15-channel stride is kept for exactly that reason; each tower now claims 8 of its 15 channels.

Resolved layout — valve channels are **1, 8, 23, 38, 53**:

```
ch  1– 4   Confluence        ch 1 = SOLENOID, ch 2–3 unwired, ch 4 unclaimed
ch  5– 8   Tower 0 decoder   strips R/G/B (75% cap) + ch  8 = FIRE
ch  9–12   Tower 0 uplight   R/G/B/W (full brightness)
ch 13–19   unclaimed
ch 20–23   Tower 1 decoder   strips R/G/B + ch 23 = FIRE
ch 24–27   Tower 1 uplight
ch 28–34   unclaimed
ch 35–38   Tower 2 decoder   strips R/G/B + ch 38 = FIRE
ch 39–42   Tower 2 uplight
ch 43–49   unclaimed
ch 50–53   Tower 3 decoder   strips R/G/B + ch 53 = FIRE
ch 54–57   Tower 3 uplight
ch 58–64   unclaimed
```

Full detail: [docs/spec-confluence-addressing.md](docs/spec-confluence-addressing.md), [docs/spec-uplight-4ch-mode.md](docs/spec-uplight-4ch-mode.md).

> NOTE: neither change fixes the Tower-0 phantom firing. The mixed uplight modes plausibly explain why towers 1/2/3 looked dead, but a valve opening on its own is still the line-noise / transmitter issue below.

---

## Session 2 (2026-07-29) — Tower 1 accumulator flicker

### Symptom
Tower 1's accumulator strips (ch 20–22) flickering, and **ch 23 — tower 1's propane
valve — carrying noise.** Gas kept off at the source throughout.

### Verified good
- Tower 1's decoder is at **A020, 4-channel mode** — read off its own display.
- Tower 1's **uplight is stable**, on the same cable at the same point in the chain →
  valid DMX *is* arriving at tower 1.
- Chain order confirmed: per tower, **decoder first, then uplight**. Tower 1 is the
  second tower in the chain, so its decoder is the first device downstream of the
  tower 0 uplight connector recorded above.

### The decisive test
Tower 1's decoder was re-mapped to read **tower 0's channels (ch 5–8)**. It still
flickered — while **tower 0's own decoder, reading the identical channels in the
identical frames, stayed clean.**

Two receivers cannot disagree about the *content* of the same bytes. This conclusively
rules out:

- ✗ Frame content and channel values
- ✗ Break misdetection / channel alignment slip
- ✗ Frame truncation or clipping
- ✗ Addressing and channel mode
- ✗ The firmware, all of it

What it does **not** rule out: identical data is not an identical *waveform*. Tower 1's
decoder sits further along the chain and sees different signal amplitude.

### The console comparison — this is the crux

| | Tower 0 position | Tower 1 position |
|---|---|---|
| **Manual console** (strong driver) | clean | **clean** |
| **ESP32** (small driver) | clean | **flicker** |

Same data in both rows. The variable is **drive strength versus position** — amplitude,
not framing. Reflections from improper termination produce exactly this
position-dependence, because standing-wave nulls sit at fixed points along the cable
and a stronger driver lifts the whole waveform back over the receiver's threshold.
See the double-termination hypothesis in **START HERE**.

### Firmware changes made in response (all flashed)

Two genuine defects were found in SparkFunDMX, and the transmit path was rewritten to
emit frames directly on `Serial1`. Full detail:
[docs/spec-dmx-transmit.md](docs/spec-dmx-transmit.md).

1. **Break was 99 µs**, only 11 µs over the spec floor → now **180 µs**
   (`BREAK_BAUD = 50000`). **Did not fix the flicker**, which the test above predicts.
2. **`update()` never flushed** the frame, so the next call's `updateBaudRate()` could
   change the divider while bytes were still in the shift register and mangle the tail.
   Now `flush()` after every frame. (This was cause #5 below.)
3. **`dmxKeepalive()` deleted** — the frame loop is now the single DMX writer.
4. **Frame lengthened 64 → 128 slots** and refresh dropped to 20 Hz, as an experiment
   against the console's 512-slot frames. **This is where the accumulator light stopped
   working** — see **START HERE**.

### Corrections to earlier conclusions in this file

- The uplights' yellow/white-runner pattern was the LL960S **built-in demo program,
  which it runs when it receives no valid DMX at all** — not a menu or addressing
  problem. Triage table added to
  [docs/manuals/strobe-lalucenatz-500w-rgbw.md](docs/manuals/strobe-lalucenatz-500w-rgbw.md).
- On the LL960S, **address and channel mode are independent settings.** The mode is its
  own menu item (`CH04`/`CH11`/`CH32`/`CH39`) defaulting to **CH11**. Setting the
  address does *not* set the mode.
- **All four uplights are LL960S.** The 9PCS 4IN1 LED Washer is a **bench test light
  only, not part of the installed rig** — its `d001`/`A001` address-prefix mode
  selection is irrelevant here. The earlier "some were 4-channel, some were 8-channel"
  observation is most likely `CH04` vs `CH11` misread, or the bench washer in the mix.

---

## Architecture fact (corrected 2026-07-29)

**There is NO outboard I2C DMX controller.** The ESP32-S3 generates DMX itself in
software on its hardware UART (`Serial1`, TX = **GPIO2**, 250 kbaud, 8N2) → an RS-485
transceiver → XLR. It is **UART → RS-485**, not I2C.

> **Correction:** this section previously said the transceiver was an SP485-class part
> on a *SparkFun LED-to-DMX shield*. That was wrong — it was an assumption from the
> library name. The hardware is the **M5Stack Unit DMX** (SKU U183) on PORTA, matching
> the `RX_PIN = 1` / `TX_PIN = 2` in `dmx.cpp` and the pinout in
> [docs/manuals/m5stack-unit-dmx.md](docs/manuals/m5stack-unit-dmx.md).
>
> Two consequences that change the action plan:
> - Its transceiver is a **CA-IS3092W with 5 kVrms galvanic isolation**, so **cause #3
>   (ground-reference offset) is already handled** — an opto-isolated booster would add
>   nothing on that front.
> - It has a **120 Ω terminator built in**, which is why double termination is now the
>   leading hypothesis.

**As of 2026-07-29 the SparkFun DMX library is no longer used at all** — it is not even
linked. Frames are emitted directly by [dmx.cpp](Test_Button_DMX/dmx.cpp).

---

## Key finding (superseded): DMX timing is spec-compliant

The SparkFunDMX library faked the break by dropping baud to ~90.9 kbaud and sending a
zero byte:

- **Break ≈ 99 µs** (spec floor 88 µs)
- **MAB ≈ 22 µs** (spec floor 8 µs)

Legal, but only just. **Now 180 µs / 40 µs** in our own transmit path — and lengthening
it did *not* fix the flicker, confirming the frame shape was never the problem.

The conclusion this section originally drew still stands: the problem is **signal
integrity / electrical**, which scales with the number of receivers. Software timing
does not scale with load; electrical drive does.

---

## 5 possible causes (ranked to the evidence)

**1. Weak / under-driven RS-485 transceiver on the shield vs. the console (most likely).**
Each tower = a unit load. A marginal SP485 on a long field run sags before its rated capacity. As towers are added, differential voltage drops until decoders misread — a misread on the fire channel opens the valve. Console driver is simply stronger.
- *Test:* scope A-B differential at Tower 0 with 1 vs 3 towers — watch the swing collapse.
- *Fix:* opto-isolated DMX booster/splitter at the ESP32 output (also fixes #3).

**2. Missing fail-safe idle biasing + enable pin never driven.**
Firmware `EN_PIN = 255` (no pin) → `pinMode`/`digitalWrite` on the transceiver DE/RE are **no-ops** ([dmx.cpp:6](Test_Button_DMX/dmx.cpp#L6); library lines 37/60/65). Direction is whatever the board hardwires. If DE isn't hardwired to always-transmit, the line goes **high-impedance between frames** — and at 50 Hz the bus is idle ~17 ms of every 20 ms. A floating idle line under noise = decoders latch garbage → self-fire.
- *Test:* scope the idle line between frames — driven solid mark, or floating?
- *Fix:* confirm/force always-TX; add 680 Ω–1 kΩ bias resistors (+5 V→A, GND→B) at the controller.

**3. Controller ground-reference offset. — RULED OUT 2026-07-29.**
The M5Stack Unit DMX's CA-IS3092W provides **5 kVrms galvanic isolation**, so the
controller's ground reference is already decoupled from the bus. This cause assumed a
non-isolated SparkFun shield that isn't in the rig.

**4. Long idle gaps + loop jitter — PARTIALLY ADDRESSED, and currently worse.**
The frame is ~5.9 ms at 128 slots; the rest of each interval is idle. Loop stalls
(FastLED, web server, morse) can make a frame late or dropped, widening the window.
The interval is now a named constant (`DMX_FRAME_INTERVAL_MS` in `dmx.h`) so this is a
one-line experiment — but it was moved the **wrong way** at the end of the session,
50 ms/20 Hz, giving a ~44 ms idle gap. Going to 10 ms or 5 ms shortens it toward
continuous, at the cost of loop time (the trailing `flush()` blocks for the frame
duration).

**5. Redundant/racing `update()` calls — FIXED 2026-07-29.**
`dmxKeepalive()` is deleted, so the frame loop is the single writer, and `dmxUpdate()`
now `flush()`es before returning so a baud change can never land mid-frame. See
[docs/spec-dmx-transmit.md](docs/spec-dmx-transmit.md).

**6. Double termination (NEW — now the leading candidate).**
The Unit DMX has a **built-in 120 Ω terminator**. A second one at the last fixture puts
**60 Ω** across the pair, halving the load impedance and cutting the differential swing
from a small driver. Explains position-dependent dropouts, the console coping, and
"more towers = more noise" all at once.
- *Test:* bus powered down, meter across XLR pins 2–3. ~120 Ω correct, **~60 Ω = two**,
  open = none.
- *Fix:* remove one terminator, or find whether the Unit DMX's built-in one is
  switchable.

---

## Field action plan — PICK UP HERE

Ordered cheapest-and-most-likely first. The firmware half is now essentially
exonerated by the two-decoder test, so these are almost all physical.

1. **Restore the baseline.** `DMX_FRAME_SLOTS = 64`, `DMX_FRAME_INTERVAL_MS = 20` in
   `dmx.h` → reflash → confirm the accumulator light works again. Nothing else can be
   measured reliably until the rig is back to a known state.
2. **Measure the termination.** Meter across XLR pins 2–3, bus powered down. Free, never
   done, and **~60 Ω would explain everything** (cause #6). Remember the Unit DMX has one
   built in.
3. **Swap tower 0's and tower 1's decoders physically.** Fault stays at tower 1's
   *position* → signal/termination. Fault follows the *unit* → that decoder is failing.
   This closes the last ambiguity in the session-2 investigation.
4. **Replace the tower 0 uplight DMX cable** and wiggle-test every XLR in the chain.
   A reseat is not a repair, and tower 1's decoder is the first device downstream of it.
5. **Scope the A-B differential at the far tower, 1 vs 3 towers.** If 2–4 come back
   clean, this is the measurement that settles drive strength (cause #1).
6. **Bias resistors, if the idle line proves to be floating** — 680 Ω–1 kΩ, +5 V→A and
   GND→B at the controller. Cannot be done in firmware: the Unit DMX's 4-pin Grove has
   **no DE wire**, so direction is whatever the module hardwires (cause #2).
7. **Only then consider a booster/splitter.** Note it would *not* fix ground offset —
   the Unit DMX is already isolated (cause #3 is ruled out).

### Open questions / TODO

- [x] Upload the Confluence CH1 change — flashed 2026-07-29.
- [x] Get all four uplights onto the same mode — all now 4-channel.
- [x] Firmware mitigation #5 (racing writer + missing flush) — fixed, flashed.
- [x] Rule out frame content / alignment as the flicker cause — the two-decoder test did this.
- [ ] **Restore `DMX_FRAME_SLOTS = 64` and `DMX_FRAME_INTERVAL_MS = 20`, reflash, confirm the accumulator light works.** ← blocking everything else
- [ ] **Measure bus termination across pins 2–3.** Free, never done, top suspect.
- [ ] Swap tower 0 / tower 1 decoders to separate unit-vs-position.
- [ ] Replace the tower 0 uplight DMX cable; wiggle-test every XLR; confirm each latches.
- [ ] Retest towers 1/2/3 solenoids once the above is done.
- [ ] Confirm whether the Unit DMX's built-in 120 Ω terminator is switchable.
- [ ] Confirm whether the Unit DMX drives DE to always-transmit, or leaves the line floating between frames (#2).
- [ ] If the 128-slot experiment is worth revisiting after the baseline is restored, try `DMX_FRAME_SLOTS = 512` to match the console exactly — one constant.
- [ ] Decide whether 20 Hz / 50 ms stays. If not, revert docs that still say 50 Hz (`CLAUDE.md`, `docs/hardware.md`, `docs/spec-dmx-transmit.md`).

> **Still open: the phantom firing / uncommanded valve opening.** Nothing this session
> fixed it. It is an electrical problem, not an addressing one — and ch 23 carrying noise
> while the firmware sends a hard 0 is the same fault in a different place. **Keep propane
> off at the source until every valve channel reads a stable 0 in idle.**

---

## Earlier general DMX-noise discussion (background)

Standard RS-485 / DMX512 field checklist that came up before the "manual console works" clue narrowed it to the transmitter:

- **Termination:** 120 Ω across A/B at the **last** fixture only. Missing terminator → reflections worsen with added cable/devices.
- **Topology:** must be a **daisy chain**, not a star / home-run.
- **Common ground:** carry pin-1 ground continuously; the differential receiver needs a shared reference (common-mode range ≈ −7 V to +12 V).
- **Shield:** ground at **one end only** (controller) to avoid a ground loop.
- **Ground potential differences** between towers on different power sources inject common-mode noise; opto-isolation is the proper fix.
- **Keep DMX away from solenoid coil / power wiring;** add snubbers/flyback diodes on solenoids to reduce switching transients.
- Quick check: measure AC/DC volts between pin-1 ground at Tower 0 vs Tower 3. >~a couple volts → ground-potential problem.

**Safety:** uncommanded propane firing is dangerous — kill propane at the source while debugging the data line.
