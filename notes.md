# Field Debugging Notes — DMX Noise / Spurious Solenoid Firing

_Session dates: 2026-07-17 → 2026-07-20, continued 2026-07-29, 2026-08-04, 2026-08-19 and 2026-09-02. Captured from the live field-debug conversations._

---

## Session 4 (2026-08-19) — Audio-reactive feature built and bench-tested on hardware

Not a DMX-noise session. Built the audio-reactive feature end to end and got it onto the
device. **Propane stayed off the whole session** — every assertion reads the DMX shadow
buffer over HTTP, so valve *commands* were verified without gas.

### Headline

**83 / 85 tests passing on hardware, and the suite caught four real firmware bugs plus one
regression I introduced while fixing one of them.** All 49 pre-existing tests pass — this is
the **first hardware validation of the fire-uplight and rapid-retrigger work**, which had
never run against a device.

### What was built

| Piece | Where |
|---|---|
| Spec | `docs/spec-audio-reactive.md` |
| Plan (with latency research) | `docs/plan-audio-reactive.md` |
| UDP receiver, limiter, predictor, 4 modes | `Test_Button_DMX/audio.h` / `audio.cpp` |
| Host packet encoder + fake Echo | `tools/audio-sim/audio_packet.py`, `send_features.py` |
| Test suite (36 new) | `tests/test_audio.py`, `tests/audio_sender.py` |
| Audio web tab | `web.cpp` + `tools/web-preview/index.html` |

The Echo itself is **not built** — that is phase 8. A laptop running
`send_features.py --pattern music --bpm 128` is the audio source, and that is enough to
drive lights and propane. Everything below was tested that way.

### ⚠ The four real bugs the suite found

Every one of these was found by a test, not by reading the code.

1. **Mode change mid-burn left the valve open.** Switching out of an audio mode while firing
   injected a release, but `modeClosesOnRelease(0)` is false for FIREBALL, so the FSM ignored
   it and held the valve for the rest of `fireDurationMs` — **10 s in the test**. Fixed with
   `fsmEndFireNow()`, which can only ever close.
2. **The burst ceiling did not hold — measured 4.46 s against a 3.0 s cap.** `minGapMs` is
   measured from shot *start*, so with a 3000 ms shot and a 100 ms gap the requirement was
   already satisfied when the shot ended. Mode 4 re-requested instantly, the valve reopened
   inside one DMX frame, and consecutive shots **merged into one continuous burn**. Fixed
   with `AUDIO_MIN_OFF_MS = 100` — a separate OFF-time floor measured from shot *end*.
3. **Prediction bypassed `beatMin` entirely.** The reactive path checked beat strength; the
   predicted path never did. Once a grid locked, any predicted beat fired regardless.
4. **Arming inherited a stale beat grid.** `staleMs` is 500 ms and the setup HTTP calls
   complete in ~150 ms, so arming picked up a grid locked moments earlier — different mode,
   different thresholds, possibly a different track — and the predictor could fire on it
   before evaluating a single new packet. `audioArm()` now clears grid, predictor and latches.

> **My own regression, caught within one run:** fixing #3 I gated the predictor on
> `g_features.beatStrength`, which tracks the *newest packet* — and non-beat packets carry 0.
> The predictor fires *between* beats, so it saw 0 nearly always and fired almost never
> (1 shot in 5 s). Now gated on `g_lastBeatStrength`, the last actual beat.

### ⚠ Two shop gotchas that cost real time

**A stale `monitor.sh` from 13 days earlier was still holding the serial port.** Two readers
on the USB-JTAG interface. It is almost certainly why four blocks needed retries during the
first flash, and it silently broke every serial capture.

> **Before blaming the cable or Bluetooth, run `lsof /dev/cu.usbmodem*`.** More than one
> holder means fix that first.

**macOS will not auto-rejoin the device AP after a reboot.** It deprioritizes a network with
no internet route, so every flash strands the test run and the laptop must be reconnected by
hand. Not fixable from our side; just budget for it.

### Upload path — no-reset strategy VERIFIED on hardware

`flash.sh`'s no-reset block strategy was marked *untested* in CLAUDE.md. It works:

- 18 blocks, all hash-verified, **~0.5 s each at ~1.05–1.2 Mbit/s** — roughly **9 s of
  transfer against the 461 s baseline**
- 4 blocks reported `attempt 1 failed` and succeeded on retry, so the **per-block retry is
  load-bearing, not belt-and-braces** (and see the stale-monitor note above)

**`flash.sh` no longer guesses the port.** With more than one `cu.usbmodem*` attached it
errors and lists them; override with `DMXFIRE_PORT=`. The chosen port is now pinned for the
whole run, so the mid-flash re-detects cannot land on a different board. This matters the
moment a second ESP32 is on the bench — flashing the wrong firmware onto the board wired to
the solenoids is not a recoverable mistake.

### Test-harness change worth knowing about

`tests/api.py` now talks to the device over **urllib3 rather than requests**, and binds the
source address. On a multi-homed laptop (device AP *plus* a network holding the default
route) macOS scopes the route to the AP interface, and unbound `requests` calls time out
while raw sockets and urllib3 both succeed from the same process at the same moment.

> **Caveat, recorded honestly:** that measurement was taken while the WiFi link was flapping,
> so I am not fully confident `requests` was at fault rather than the network. The urllib3
> transport works and is no worse, but it is a candidate for reverting once the link is
> stable for a full run.

### Confluence is marked disconnected in NVS

Boot diagnostics report `[FAIL] Confluence marked disconnected — solenoid will not fire`.
Pre-existing, not from this work. Tests are unaffected (the baseline fixture sets it true),
but **manual** firing will leave CH1 at 0 and only the four tower valves respond.

### Still open

- **`audlead` is a guess (120 ms) and must be measured.** Film one shot at ≥120 fps with the
  status LED in frame and count frames to visible flame. Until then, beat sync is uncalibrated.
- Echo firmware (phase 8), blocked on the M5Unified **0.2.4 sketchbook vs 0.2.13 repo** split,
  which needs its own commit.
- OTA still unproven end to end; USB remains the working path.
- Nothing has been fired with gas. Steps 1–4 of the shop sequence were gas-off only.

---

## ⚠⚠ SESSION 5 (2026-08-19) — "Tower 1 is dead" was a TEST FIXTURE, not hardware

> **The same symptom had a different cause in Session 6 (2026-09-02):** a cable that passed a
> pin-by-pin tester, with a completely clean config. Check both — and read Session 6's extension of
> the standing rule before trusting a neighbouring fixture as proof of signal.

**Read this before diagnosing any dead fixture again.** Tower 1's accumulator decoder was
reported dead — strips unlit, solenoid silent — while its uplight worked and towers 0/2/3 were
fine. A manual DMX console drove the decoder perfectly. The decoder had already been
**physically replaced**, and the cable had **passed a pin-by-pin cable tester**.

### What it actually was

`towerConfigs[1].connected == false`, persisted in NVS.

[Test_Button_DMX.ino:209](Test_Button_DMX/Test_Button_DMX.ino#L209) —
`if (!towerConfigs[i].connected) continue;` — skips `towerWrite()` for that tower entirely, so
**CH20–27 are never written and sit at zero forever.** Ticking Connected in the web UI and
pressing **Save** restored it instantly; CH20–27 started animating on the sniffer that second.

### The measurement that found it

Enttec RX monitor (`tools/dmx-tester/index.html`) tapped in right after the M5, 20 samples over
8 s with themes running:

| Tower | decoder strips | uplight |
|---|---|---|
| 0 | CH5/6/7 = 9/9/3 | CH9/10/11 = 12/12/4 |
| **1** | CH20/21/22 = **0/0/0** | CH24/25/26 = **0/0/0** |
| 2 | CH35/36/37 = 24/42/21 | CH39/40/41 = 32/56/28 |
| 3 | CH50/51/52 = 54/55/58 | CH54/55/56 = 73/74/78 |

Tower 1 zero in *every* sample while its neighbours carried live data **in the same frames**.
That rules out cable, connector, termination, ground reference, drive strength and the decoder
in one shot — a transmitter that never sends the bytes cannot be an electrical fault.

### Where the flag came from

Device config was a **byte-for-byte match for the write set of
[tests/test_storage.py](tests/test_storage.py)** — towers `fire/11/120/22`, `blue/33/80/44`,
`green/55/200/66`, `blue/77/300/88`, confluence `connected=False, fireLevel=99`, button
`mode 1 / 1500 / 3000`. That test deliberately writes `connected=False` to tower 1 and the
Confluence to prove distinct values round-trip, and it sorts **last** in the suite.

`conftest.py`'s `baseline` fixture reset config *before* each test but **never after**, so the
last test's writes went to NVS and outlived the run — surviving reboots, looking exactly like a
dead decoder. **Fixed:** `_apply_baseline_config()` now runs in the fixture teardown as well as
setup, so a run can no longer leave a fixture disconnected.

> **Caveat on causality.** The operator recalls only running the API suite *recently*, while
> Tower 1 had been misbehaving since Session 2 (2026-07-29). So this almost certainly does
> **not** explain the original flicker — that was the electrical fault of Sessions 2–3. What is
> established by direct measurement is narrower and still decisive: on 2026-08-19 the flag was
> `false`, the M5 was sending zeros to that tower because of it, and clearing the flag brought
> the tower back. A test run during the debugging almost certainly introduced a *second*,
> independent failure on top of the first, and it is the one that made the tower look dead.

> **The Confluence was disabled the same way** (`connected=false` →
> [.ino:252](Test_Button_DMX/Test_Button_DMX.ino#L252) skips `confluenceWrite()`, central
> solenoid never fires), and kept the test's `fireLevel=99` after the towers were restored.

### Why it hid for so long

- The failure is **silent** in normal operation — the `[DMX]` serial log prints only CH1–5 and
  only on FSM transitions. The boot diagnostic *does* catch it
  ([tests.cpp:44](Test_Button_DMX/tests.cpp#L44) `FAIL("Tower N is marked disconnected")`) but
  only on the serial console, and nobody was watching it.
- **The web UI lied.** Config forms were server-rendered once at page load, so the checkbox
  could show Connected while NVS said otherwise — and since the form posts every field, saving
  a stale form writes the stale values back. **Fixed this session:** the Towers and Confluence
  forms now poll `/api/state`, sync themselves from the device, pause syncing while you are
  editing, and print the live on-air bytes with a loud banner when a fixture is skipped.
- Every symptom was a **perfect forgery of an electrical fault**, and this rig has a real,
  documented electrical history (Sessions 2–3), so the priors pointed the wrong way.
- The manual-console test "proving" the decoder good was really proving that *the console sends
  bytes on CH20–23 and the M5 does not.*

### Also corrected this session

**`flameLevel` / `fireLevel` are not proportional flame controls.** The solenoid is an on/off
valve; the byte only has to clear the decoder's turn-on threshold to energise the coil. Low
values leave it shut or chatter it. Flame *size* is gas pressure and orifice, not DMX. The
"0=off, 255=full open" comment in `towers.h` said otherwise and has been rewritten.

**A fire duration below one DMX frame cannot light.** The bus runs at 20 Hz, so
`fireDurationMs < 50` can only ever reach the wire as a single 50 ms frame — the rig was found
on `fireDurationMs=10`. The UI now warns on this.

**The Confluence kept the test's `fireLevel=99`** even after the four towers were restored to
255, so the central solenoid stayed weak once it was re-enabled. Worth checking separately from
the towers: they are restored by different forms.

### New tooling

`scripts/towers.sh` — prints every tower's persisted config beside the live DMX bytes for its
decoder and uplight blocks, and **flags any setting that blanks a fixture**
(`connected=false`, `brightness=0`, `flameLevel=0`, disconnected Confluence). Run it **first**,
before touching a cable. It is what found this in about ten seconds.

**Live fixture state in the web UI** — the config forms were server-rendered once at page load
and never refreshed, so the page could show *Connected* ticked while NVS said otherwise, and
saving a stale form wrote the stale values back. They now poll `/api/state`, hydrate from the
device, pause while you are editing, print the live on-air bytes, and raise a loud banner for
every silent failure (disconnected, `brightness=0`, `flameLevel=0`, sub-frame fire duration).
[docs/spec-live-fixture-state.md](docs/spec-live-fixture-state.md).

**DMX quiet mode / bus handover** — `POST /api/dmx/quiet/start|stop` and a control in the Tower
Configs tab stop the transmitter so a manual console or the Enttec can drive the bus without
unplugging the M5 (DMX has no arbitration; two transmitters garble each other). Reuses OTA's
safety helpers — refuses unless the rig is idle, and drives every valve shut **on the wire**
before going silent, because fixtures latch their last commanded value when frames stop.
Global by necessity: a frame carries all 64 slots, so there is no per-tower quiet.
RAM-only and never persisted — a saved "stop transmitting" flag would be the same trap as a
saved `connected=false`. [docs/spec-dmx-quiet-mode.md](docs/spec-dmx-quiet-mode.md).

**Valve-channel logging** — the `[DMX]` line printed slots 1–5 with RGB labels left over from an
older channel map, so it was blind to the tower valves and to purge. It now prints
CH1/8/23/38/53 plus the frame counters — which turned out never to have been declared in
`dmx.h`, despite a comment in `dmx.cpp` claiming they were exposed for exactly this.

### Standing rule

> **Before diagnosing a dead fixture as hardware, confirm the controller is actually sending
> bytes to it.** `scripts/towers.sh`, or the RX monitor at the head of the chain. Sessions 2–5
> collectively burned a decoder, a cable test and several field days on faults that a
> thirty-second transmit check would have separated immediately.

Nothing here overturns Session 3's ground-reference root cause — that was a genuine, separate,
electrical fault with its own evidence. This is a second, independent failure mode layered on
top of it that mimicked it.

---

## ⚠⚠ SESSION 6 (2026-09-02) — Tower 1 again; the cable was NOT the root cause — the decoder was

> ## ⚠⚠⚠ CORRECTION (2026-09-02, from the operator — read this before the section below)
>
> **The root cause was the DMX decoder, not the cable.** The operator replaced the tower 1
> decoder and that is what fixed it. The cable swap recorded below did not resolve the fault.
>
> Everything in this section written as "the cable was the fault" is **wrong**, and the
> reasoning built on it — the three mechanisms for why a bad cable spares some fixtures, the
> "a cable that passes a tester is not exonerated" conclusion, the inference that
> `docs/hardware.md`'s topology must be wrong — is reasoning from a false premise. The
> *measurements* below are still good and still worth reading; the *diagnosis* is not.
>
> **What actually holds up:**
> - The transmit side was provably healthy (CH20–23 carried correct animating data), so the
>   fault was downstream of the controller. That much was right and is what mattered.
> - Session 5's rule held: confirming the controller is sending is necessary, not sufficient.
> - The decoder's `- - -` display check is still the right first move — and had it been done,
>   it would have pointed at the decoder immediately.
>
> **The standing lesson is the opposite of the one drawn below.** Not "a cable that passes a
> tester is still suspect" — but: *swapping a part and seeing the symptom clear does not
> establish that part was the fault, if other things changed too.* Two components were
> touched here across the session and the first swap got the credit.
>
> This is now the **third** time tower 1 has been diagnosed wrong (Session 2/3: electrical;
> Session 5: a config flag; Session 6: a cable) before the real cause was found. Every one of
> those had confident supporting evidence at the time.
>
> **The "Open questions" list at the end of this section is affected.** The cable items
> ("which cable was replaced", "meter it for pin 1/shield before binning it") are no longer
> chasing a known fault — though metering it is still cheap, and if that cable *does* turn out
> defective it would mean two faults were present at once, which is worth knowing. The
> `docs/hardware.md` topology item was raised only because the cable diagnosis implied the
> drawn chain was wrong; with the decoder as the cause, that inference no longer stands and
> the diagram is neither confirmed nor refuted. The `ota.sh` fallback item is **done** —
> see Session 7.

**The section below is retained as written, for its measurements and its ruling-out work.
Its conclusion is superseded by the correction above.**


**Read this together with Session 5.** The presenting symptom was identical, down to the detail:
tower 1's accumulator decoder inert (strips unlit, valve silent), **its uplight running the theme
normally**, towers 0/2/3 fine, and a manual DMX console driving the decoder perfectly at ch 20.

Session 5's answer was `connected=false` in NVS. **This time the config was clean and the fault was
physical.** The operator replaced a cable and the tower came straight back. That cable had **passed
a pin-by-pin cable tester** — the second time in this rig's history a cable test has given false
confidence.

### What it was NOT — ruled out by measurement before touching hardware

| Ruled out | Evidence |
|---|---|
| Session 5's config fault recurring | `scripts/towers.sh`: all four towers `connected=true`, brightness 192, flameLevel 255; Confluence connected, fireLevel 255. No blanking config anywhere. |
| Firmware not composing the bytes | CH20–23 carried live animating theme data (e.g. `74/95/0/0`) every sample. `74/99 = 0.747` — exactly the 75 % `STRIP_BRIGHTNESS_PCT` cap against the uplight's `99`, so the value was provably correct by construction, not a coincidence. |
| Firmware composing but not sending | `dmxUpdate()` writes `dmxLastFrame` to the UART itself — the same buffer `/api/state` reports. For slots 1–64 there is no shadow-vs-wire divergence possible. |
| Wrong address | Decoder confirmed set to **A020**, and the console driving **ch 20 specifically** controlled it. |
| Frame length | Was briefly the leading theory. Wrong, and backwards: `notes.md` already records that lengthening 64 → 128 slots is **what killed this same fixture** in the first place (reverted 2026-08-05). |

### The part worth remembering: a bad cable did NOT kill the whole downstream chain

Tower 1's decoder was stone dead while its **uplight kept running the theme correctly**, and towers
2 and 3 downstream were unaffected. A single fixture went dark and everything around it carried on.

**Do not turn this into a hop-count rule.** "It only affects one node downstream" is not established
and probably is not consistent. A partially-faulted cable does not fail cleanly — it produces a
*degraded* signal, and whether any given fixture still works depends on **that fixture's receiver
and its grounding**, not on how far down the run it sits. Three mechanisms produce this pattern, and
we have not yet established which one was in play here:

1. **It isn't actually a chain at that point (spur / star tap).** If the decoder hangs off a tap
   rather than sitting in-line, its cable carries data to nothing else, so nothing else can notice
   it fail. Simplest explanation, and it would mean the daisy chain drawn in
   [docs/hardware.md](docs/hardware.md) does not match the field.
2. **Pin 1 / shield broken while pins 2–3 stayed continuous.** This ties straight to **Session 3's
   confirmed root cause** — the M5 Unit DMX is galvanically isolated and cannot reference the bus,
   so the reference has to come from the fixture end. Lose pin 1 on one leg and the fixtures past it
   are left floating. A mains-earthed fixture (the LL960S uplight is a 500 W earthed unit)
   self-references and rides it out; a decoder on a floating low-voltage DC supply does not. This
   mechanism is **fixture-dependent, not position-dependent** — which is exactly what we saw.
3. **A marginal conductor and differing receiver tolerance.** Series resistance from a cold joint or
   part-severed strands slew-limits the edges. RS-485 receivers differ in hysteresis and sampling
   margin, so a tolerant fixture decodes the same degraded waveform that a strict one rejects.

Mechanisms 2 and 3 both predict "some fixtures survive, some don't" **with no relationship to
downstream order**. So the usable rule is not about hops:

> One fixture dark, its immediate neighbours fine, and a console that drives it correctly = suspect
> the cable segment feeding **that fixture**, and swap it. Do not try to reason about who is
> downstream of whom — bypass the cable and observe.

### Why the cable tester passed a cable that was dead

A pin tester checks DC continuity and pin mapping at a few volts and about a milliamp. DMX is
250 kbaud differential signalling on a nominally 110 Ω line. **Nothing in a continuity test
exercises rise time, impedance, or the shield.** Specifically:

- Cold joints and part-severed strands conduct fine at 1 mA and collapse under signal edges.
- Intermittents that only open under flex or tension read perfect on a bench where the cable is
  lying still.
- Mic cable (~50–75 Ω, high capacitance) substituted for 110 Ω DMX cable passes every continuity
  check and reflects badly at 250 kbaud.
- Many testers do not meaningfully test **pin 1 / shield** — see mechanism 2 above, where pin 1 is
  the whole fault.

**A cable that passes a tester is not exonerated.** Swapping it is the test; the tester is not.

### Correction to a conclusion drawn earlier in this session

Mid-session it was argued that the decoder *must* be receiving our frames, because the uplight drawn
**downstream of it** in [docs/hardware.md](docs/hardware.md) (`controller → … → T1 decoder → T1
uplight → T2 decoder → …`) was working normally. The cable fix disproves that. Either the wiring is
not the chain that document draws, or the decoder's feed is a separate leg from the uplight's.
**The topology diagram in `docs/hardware.md` should be treated as unverified until someone traces
the actual cables.**

### The ten-second check that would have split this open immediately

Per [docs/manuals/dmx512-decoder.md](docs/manuals/dmx512-decoder.md), the accumulator decoders show
**`- - -` on the display when no DMX signal is being received.**

That single reading separates *"not receiving"* from *"receiving and ignoring"* — the exact fork that
consumed this session and Session 5 both. **Look at the decoder's display before theorising.**

### Standing rule — extended

> Session 5: **Before diagnosing a dead fixture as hardware, confirm the controller is actually
> sending bytes to it.**
>
> Session 6: **That is necessary but NOT sufficient.** Confirm *that fixture* is receiving them —
> its own display, or a sniffer at the fixture. **A neighbouring fixture working is not proof the
> signal reaches this one**, no matter what the topology diagram says.

### Also done this session

- **The device was three commits behind.** It was running `9f89f10` (audio-reactive): `/api/state`
  returned no `dmx.quiet` key, so Session 5's fixes — the live-syncing web UI, DMX quiet mode and
  valve-channel logging — **had never been flashed**. The operator was still looking at the old
  server-rendered UI that Session 5 caught lying. OTA'd to HEAD `6524257` this session; NVS config
  survived, as designed.
- **`curl` cannot talk to this device right now; raw sockets can.** Every variant (HTTP/1.0, 1.1,
  `Connection: close`, stripped `Accept`/`User-Agent`) returns an empty body, while a plain socket
  and `nc` return valid JSON, and ping is 0 % loss. `scripts/towers.sh` and `scripts/debug.sh`
  already carry an `nc` fallback for this; **`scripts/ota.sh` does not, and its single un-retried
  `curl` pre-check blocks the upload entirely.** Worked around with a raw-socket multipart POST.
- **`fireDurationMs` was found at 60 ms**, i.e. barely one 50 ms DMX frame — a valve pulse far too
  short to produce visible flame, on every tower at once. It caps `FIRE_ACTIVE` in *every* mode
  ([button_fsm.cpp:81](Test_Button_DMX/button_fsm.cpp#L81)), so holding the button does not extend
  it. Same class of trap as the `fireDurationMs=10` found in Session 5. The operator raised it and
  confirmed the valves are audibly opening, and will tune flame size in the field.

### Open questions

- [ ] **Which cable was replaced, and where in the run?** Not recorded. This is what separates
      mechanism 1 (spur) from mechanisms 2/3 (degraded in-line signal) — and they imply different
      fixes for the rest of the rig.
- [ ] **Keep the bad cable and meter it — do not bin it.** It is the only physical evidence. Check
      **pin 1 / shield continuity** specifically, and flex it while metering. If pin 1 is the fault,
      that is Session 3's root cause resurfacing as a cable defect, and the rest of the run should be
      checked the same way.
- [ ] Trace the actual cabling at tower 1 and correct `docs/hardware.md` if it is not the drawn chain.
- [ ] Give `scripts/ota.sh` the same `nc`/raw-socket fallback `towers.sh` and `debug.sh` have.
- [ ] Re-check whether any of the remaining cables are mic cable rather than 110 Ω DMX cable.

---

## Session 7 (2026-09-02) — Fire is binary: valve channels are 0 or 255, and nothing can write anything else

_Ran **concurrently** with Session 6 above, on the same rig — that session had the device,
this one had the tree. Read Session 6 for the tower-1 cable fault; this entry is the
firmware change and does not bear on it._

Not a noise session. A design change, plus one real bug found on the way.

### Headline

**`flameLevel` and `fireLevel` are gone.** A valve channel now carries `0` or `255` and
nothing else, enforced in the type system *and* at the DMX write. Per-fixture propane
isolation survives as a boolean, `fireEnabled`.

This is not a new discovery — Session 5 already recorded that these bytes were not
proportional flame controls. What changed is that the rig stopped *offering* the setting.

### Why the old design was wrong

The byte reached CH 1 / 8 / 23 / 38 / 53 **verbatim**; no scaling existed anywhere in the
path. So the slider never made a smaller flame. All it decided was whether the decoder's
turn-on threshold was cleared. Below it, the valve stayed shut or the coil chattered.
Flame size is gas pressure and orifice.

Every layer of the codebase had been papering over this for months:

| Where | What it said |
|---|---|
| `towers.h` | comment: "NOT a proportional flame control" |
| web UI live readout | warned on any value `< 128` |
| `docs/spec-live-fixture-state.md` | carried a standing correction |
| `notes.md` Session 5 | corrected it again |
| `scripts/towers.sh` | existed partly to flag `flameLevel=0` |

> **A control that every layer has to warn about should not exist.** Five separate
> warnings are not documentation, they are a design smell with a long paper trail.

### How the rule is enforced — three independent layers

Deliberately redundant, so a future refactor cannot quietly lose it.

1. **Types — a partial value is unrepresentable.**
   `TowerState.fire` (`uint8_t`) → `TowerState.fireOpen` (`bool`);
   `confluenceWrite(uint8_t level)` → `confluenceWrite(bool open)`;
   `morseTick()` returns `bool`. Every missed call site became a compile error, which is
   the reason the change was shaped this way.

2. **The DMX write — the backstop.** Valve channels are now declared as **data** in
   `dmx.h` (`VALVE_CHANNELS = {1, 8, 23, 38, 53}`), and `dmxShadowWrite()` refuses any
   other byte on one and logs it at ERROR. `dmxValveWrite(ch, bool)` is the intended
   interface; there is no level to pass.

3. **The UI and API — no field to set.** Both sliders deleted, `/set` reads no level,
   `/api/state` reports none.

**Refuse, not clamp.** A caller that computed 128 for a solenoid has a bug, and guessing
which way to round it is guessing about propane. Dropping the write leaves the channel at
whatever it last held, and every path that opens a valve rewrites it every frame — so the
safe steady state is already on the wire.

### Valve-ness used to be declared nowhere

Before this, the firmware could not answer "is channel N a valve?". Valve-ness was an
offset convention (`base + 4`, `base = 4 + i*15`) plus the literal `1`, restated by hand in
**six** places: the `[DMX]` log line, `tests.cpp`, `web.cpp`'s browser JS, `scripts/towers.sh`,
`tools/dmx-tester/index.html` and three separate test modules. Only the browser tool's copy
was machine-readable, and it talks to an Enttec, not to the firmware.

`testValveChannelMap()` now runs at boot and checks that `towers.cpp`'s stride and
`dmx.cpp`'s registry still agree — so the two statements of the same fact cannot drift apart
silently. `tests/valves.py` collapses the four Python copies onto one.

### ⚠ Real bug found: a disconnected fixture stranded its valve OPEN

Directly related to Session 5's `connected=false` finding, and worse.

A fixture marked `connected=false` was skipped in the frame loop — and **a DMX channel that
stops being written keeps its last byte.** So:

> Un-tick "Connected" on a tower **during a burn**, and its solenoid stays latched at 255
> with nothing left to close it. Same hazard on CH1 for the Confluence.

Session 5 read `connected=false` as a fixture that goes *dark*. It is that — but only
because the channels happened to be sitting at zero. Mid-fire it is the opposite failure,
and it is the dangerous direction.

Fixed: both skip paths now drive the valve shut explicitly *before* skipping. The lighting
channels still go dark, as before — only the valve is forced.

```cpp
if (!towerConfigs[i].connected) {
  dmxValveWrite(towerValveChannel(i), false);   // never leave a valve latched open
  continue;
}
```

Two host tests cover it (`test_disconnected_tower_valve_is_forced_closed`, and the
Confluence equivalent), and `scripts/towers.sh` no longer claims the valve merely "stays
at 0".

### `fireEnabled` — why not just reuse `connected`

`connected` is too blunt. Unticking it blanks the whole fixture, which on the wire is
indistinguishable from a dead decoder or a broken cable — **the exact forgery that cost
Sessions 2–5 a decoder, a cable test and several field days.** Turning off one tower's
propane should not require reproducing that failure mode.

`fireEnabled` isolates *only* the gas: lights keep running, so an isolated tower still
looks alive on stage. It gates every source alike — button, purge, Morse, audio — so one
flag isolates a fixture from all of them.

Deliberately **not** on the Apply-to-All form, for the same reason `connected` isn't: a
browser submits nothing for an unchecked box, so one "Apply to All" from a form without
that box would clear the flag on all four towers at once.

### NVS: new keys, old ones actively removed

| Key | Type | Default |
|---|---|---|
| `t<N>v` | bool | `true` |
| `cffe` | bool | `true` |

The retired `t<N>f` / `cffl` keys are **not reused**. They hold `UChar` entries, and
`Preferences::getBool()` on a type-mismatched key silently returns the default — too quiet
a behaviour to rest a propane setting on. `storageSave()` calls `prefs.remove()` on both,
so a rig that has run the old firmware sheds them on its first config write. No
`scripts/flash.sh --erase` needed.

### ⚠⚠ Shop gotcha: a shared working tree can silently disable every valve

Found while coordinating with the parallel session, **not** by hitting it — but it is the
same shape as the Session 5 trap and deserves the same prominence.

The host test harness and the firmware are versioned together but **run separately**. During
this session `tests/api.py` was already converted to the new signatures while the device was
still on old firmware. In that window:

- `set_tower()` no longer sends `flameLevel` at all.
- Old firmware does `towerConfigs[idx].flameLevel = server.arg("flameLevel").toInt()`.
- An absent arg gives `""`, and `"".toInt()` is **0**.
- `storageSave()` persists it. `conftest`'s teardown re-applies the same thing.

**Result: running pytest from an updated tree against older firmware sets every tower's
flameLevel to 0 in NVS, across reboots — and it looks exactly like "the valves are dead".**

> **Standing rule.** The test harness and the firmware on the device are one unit. Never
> run the API suite from a tree that is ahead of (or behind) what is flashed. If you must
> test old firmware, check out the matching `tests/` — a worktree is the cheap way.

This generalises past `flameLevel`: **any** field the firmware reads with `server.arg()` or
`hasArg()` and the client stops sending will silently take its zero/false value. `connected`
and `fireEnabled` are both checkboxes and both behave this way by design.

### Testing added

**On-device**, printed by `runDiagnostics()` at boot over serial at 115200:

- `testValveChannelMap()` — every tower valve derived from the stride is in
  `VALVE_CHANNELS`, CH1 is registered, and no colour channel is.
- `testValveGuardRefusesPartial()` — writes 1 / 64 / 127 / 128 / 200 / 254 at a live valve
  channel and asserts none of them land; then confirms both legal values do, and that a
  neighbouring colour channel still takes 128. Safe because nothing in it calls
  `dmxUpdate()` and `runDiagnostics()` runs before `loop()` starts — **do not add a
  `dmxUpdate()` to that function.**

**Host** — `tests/test_valve_binary.py`, 14 tests: valve bytes are 0/255 under FIRE_ACTIVE,
purge, MACHINE_GUN, Morse and a full FSM cycle; MACHINE_GUN pulses between *exactly* those
two values (it gates time, never amplitude); a legacy `flameLevel=200` post is inert;
`fireEnabled=false` shuts the valve while the uplight still shows the fire look;
disconnecting a fixture mid-burn drives its valve to 0; and a non-valve channel still
carries a mid-scale byte — the guard must not overreach onto the lights.

The END_CUE white fade is the one place a ramp is deliberately generated. It lives on the
uplight's white channel, and `test_valves_binary_across_a_full_fsm_cycle` exists to prove
none of it leaks onto a valve.

### Also changed

- `tools/dmx-tester/index.html` — the Enttec tester drives the **real** solenoids, so
  `setCh()` now enforces the same rule the firmware does.
- `scripts/towers.sh` — `flame` column became `fire` (enabled/off); dropped the
  `flameLevel=0` check; **added a flag for any valve byte that is neither 0 nor 255**,
  which after this change would mean the firmware guard has been bypassed.
- `tools/web-preview/server.py` — the mock never modelled purge or MACHINE_GUN in
  `compose_dmx()`, so the preview showed all valves shut during a purge. Now it does, and
  `/api/state` reports `purge` like the firmware does.
- Web UI live readout — the `< 128` / `=== 0` warnings are **deleted, not reworded**. In
  their place: a `fireEnabled === false` notice, plus `vf()`, which flags a non-binary valve
  byte as a firmware fault rather than a setting to explain.
- **`scripts/ota.sh` now has the raw-socket fallback** that `towers.sh` and `debug.sh`
  already had — closing the open item Session 6 raised after having to hand-roll a POST.
  curl currently returns an empty body against this device while sockets and ping are fine,
  and ota.sh's **single un-retried curl pre-check hard-failed and blocked the upload
  entirely**. Now every request tries curl, falls back to a plain socket, and the pre-check
  retries three times before giving up. python3 rather than `nc` for the upload, because the
  multipart body is binary and has to be framed exactly. Verified against a local endpoint
  with a 1,189,852-byte payload: byte-exact, SHA-matched, over the socket path with curl
  forced to fail.

Spec: [docs/spec-solenoid-binary.md](docs/spec-solenoid-binary.md).

### Flashed and verified on hardware (2026-09-02)

OTA'd over WiFi with the operator present and the rig idle — `boot_id 065411b0` →
`40431294`. **No propane: solenoid/DMX testing only.** Verified on device:

- `/api/state` carries `fireEnabled` on the Confluence and all four towers; **`fireLevel`
  and `flameLevel` are gone from the payload.**
- **NVS migration worked with no `--erase`.** `fireEnabled` came up `true` everywhere from
  absent `t<N>v` / `cffe` keys, and the operator's `fireDurationMs=590 / cooldownMs=40 /
  endCueMs=330` and `theme=simon / 255 / 110` survived the flash untouched — different keys.
- All five valve channels read 0 and stayed binary throughout.
- **31/31 config and schema tests pass on hardware**, none of which command a valve —
  `test_config_all_towers`, `test_config_confluence`, `test_config_per_tower`,
  `test_config_button`, `test_state`, `test_storage`. None of those command a valve.
- `scripts/towers.sh` reports the new `fire` column and finds no blanking config.

**Not yet run on hardware: anything that opens a valve.** `test_valve_binary.py`,
`test_dmx_output.py` and the FSM/mode suites all command valves, and the rig was mid-repair
with hardware unplugged. They are deferred to the next session. The two boot diagnostics
(`testValveChannelMap`, `testValveGuardRefusesPartial`) **did** run on this boot but print to
serial, and no USB was attached to capture them.

### ⚠⚠ The Session 5 trap fired again, on a rig in active use, during this session

Running the API suite against the rig **silently replaced the operator's field tuning**,
mid-session, while they were working at it.

Before the suite: `mode=0  fireDurationMs=590  cooldownMs=40  endCueMs=330`, all towers
`theme=simon brightness=255 speed=110`.
After: `mode=1  fireDurationMs=1500  cooldownMs=3000`, towers back to `green/128/100`.

**Cause.** `conftest.py`'s `_apply_baseline_config()` restored tower and Confluence config in
both setup *and* teardown — the Session 5 fix — but **button config and the fire-uplight
colour were only ever applied in the setup half.** `test_storage.py` writes
`mode=1 / 1500 / 3000` to prove distinct values round-trip and **sorts last**, so its button
write was the final NVS write of the run and outlived it.

Identical failure shape to Session 5 (a test's writes surviving as device config), on a
different field, and the Session 5 fix did not generalise because it enumerated fields
rather than covering the category.

**Fixed:** `_apply_baseline_config()` now owns the whole baseline — button config and fire
uplight included — so setup and teardown apply the identical set. Values were restored to
the operator's readings within a minute.

> **Standing rule.** `conftest`'s teardown must restore **everything a test can persist**,
> not a hand-picked subset. Every new persisted field is a new instance of this bug. When
> adding one to `/set` and NVS, add it to `_apply_baseline_config()` in the same change.

> **And the sharper operational rule:** even with the fix, **running the API suite resets
> the rig to test baseline.** That is now deterministic rather than "whatever the last test
> wrote", but it is still not the operator's tuning. **Do not run the suite against a rig
> carrying field tuning you care about without recording `/api/state` first.**

### Standing rule

> **A solenoid channel is binary. There is no level, there was never a level, and no future
> feature gets to add one.** If a rig ever gets a proportional gas valve, that is a new
> fixture type with its own spec — not a level byte on a valve channel. The guard in
> `dmxShadowWrite()` is scoped to those five channels and must never widen onto the lights.

### ⚠ Standing assumption: NO propane unless the operator says otherwise

Recorded because it was got wrong this session. A peer session reported the operator was
"fire-testing with propane flowing" and "can hear the valves opening"; that was read as live
gas and written into an OTA risk assessment. **There was no propane at any point.** The work
was dry solenoid and DMX testing — solenoids clicking, bytes on the wire.

> **Default to gas OFF.** Do not infer live propane from "fire testing", "tuning flame size",
> or "the valves are opening" — those all describe dry solenoid work. The operator is the
> only reliable source on gas state, and every session in this file so far has been dry
> (Session 4 says so explicitly). If a decision genuinely turns on it, ask.

Assuming gas that is not there is not a harmless conservative error: it produced false
urgency, distorted a technical decision, and got recorded as fact in these notes until the
operator corrected it.

### Rig state at end of session (2026-09-02)

- Running the binary-valve firmware, `boot_id 40431294`. NVS carries the new `t<N>v` / `cffe`
  keys; the retired `t<N>f` / `cffl` were removed on first save.
- Button config restored to the operator's values: `mode=0 fireDurationMs=590 cooldownMs=40
  endCueMs=330`. Themes were being changed at the web UI as the session closed.
- **Tower 1's DMX decoder is being replaced** — hardware unplugged, rig mid-repair.
- **No tests were run after the repair began**, and nothing that commands a valve has run on
  this firmware at all. That is the first job next session.

### Reconciling with Session 6 (concurrent, same rig)

Session 6 investigated tower 1 going dark and initially concluded a bad cable; **the operator
later established it was the DMX decoder, which was replaced.** See the correction at the head
of that section. Recorded here only so the two writeups are not read as one investigation.
Three points where they touch:

**Session 5's standing rule held, and was not sufficient.** "Confirm the controller is
actually sending bytes" did its job — `scripts/towers.sh` came back clean (all towers
`connected=true`, `flameLevel=255`) and CH20–23 carried correct animating data, which correctly
placed the fault downstream of the controller. It could not narrow further than that, and the
first guess past it (the cable) was wrong.

**The shop gotcha above did NOT bite them.** Session 6 read `flameLevel 255` across all four
towers, so no pytest run from the updated tree had polluted NVS before their measurements.
The hazard was real but did not fire; it is recorded because the window is still open for
anyone else running a mixed tree.

**One thing this change makes moot.** Session 6 lists "`fireDurationMs` found at 60 ms" as
the same class of trap as Session 5's `flameLevel=0` — a silent config value that stops
flame without looking broken. `flameLevel` is now gone as a member of that class; there is
no valve *level* left to find misconfigured. `fireDurationMs` remains, and the sub-frame
warning in the web UI still covers it.

> **Do not read this change as bearing on the cable diagnosis.** Nothing here would have
> found or prevented it. The one overlap is the latched-valve bug above: had tower 1 been
> un-ticked mid-burn during that debugging, the old firmware would have stranded its valve
> open. It was not, but that is luck rather than design.

---

## ⚠ START HERE — device state as of 2026-07-29

**ROOT CAUSE FOUND (Session 3, 2026-08-04) — see the Session 3 write-up below.** The whole
solenoid saga is a **missing bus ground reference**: the M5 Unit DMX is galvanically isolated,
so it gives the bus no ground, and the decoders' RS-485 receivers drift out of common-mode
range and misread the valve channels. A grounded device on the bus (an Enttec) fixes it just
by being connected. **The double-termination theory below is overturned** — adding a grounded
device *helped*, so the bus was under-referenced, not over-loaded. The earlier 128-slot
accumulator-light regression noted here was also fixed (slots reverted to 64, flashed 2026-08-05).

### What is on the device right now

| Setting | Value | Where |
|---|---|---|
| Confluence solenoid | **CH1** | `confluence.cpp` |
| Uplight mode | 4-channel R/G/B/W | `towers.cpp` |
| Tower fire valves | decoder CH4 → ch **8 / 23 / 38 / 53** | `towers.cpp` |
| Break / MAB | **180 µs / 40 µs** | `dmx.cpp` — `BREAK_BAUD = 50000` |
| Frame slots | **64** (reverted from 128, 2026-08-05) | `dmx.h` — `DMX_FRAME_SLOTS` |
| Refresh | **20 Hz** (50 ms) | `dmx.h` — `DMX_FRAME_INTERVAL_MS` |
| Addressed channels | 64 | `dmx.h` — `DMX_SHADOW_SIZE` |
| DMX writer | single (frame loop only) | `dmxKeepalive()` deleted |
| DMX bus ground ref | **NONE from the M5** (isolated source) | root cause — Session 3 |

### Status (updated 2026-08-04)

- The 128-slot regression is **fixed** — `DMX_FRAME_SLOTS` reverted to 64, flashed 2026-08-05.
  Rate kept deliberately slow at 20 Hz; a `dmxReadyToSend()` TX-drain guard added.
- **The solenoid saga's root cause is found (see Session 3 below): a missing bus ground
  reference, caused by the M5 Unit DMX's galvanic isolation.** The fix is a grounded
  re-driver or fail-safe bias on the fixture side — not a firmware change.
- The **double-termination (60 Ω) hypothesis is overturned** — adding a grounded device (the
  Enttec) *fixed* the bus, so it was under-referenced, not over-terminated. Metering pins 2–3
  is still harmless to do, but it is no longer the top suspect.

---

## Session 3 (2026-08-04) — ROOT CAUSE FOUND: isolated source, no bus ground reference

_The solenoid saga is solved (diagnosis). Everything below is confirmed by direct observation
and a multimeter, not theory._

### The decisive observation

With an **Enttec DMX USB Pro** connected anywhere on the bus, the whole rig works — including
the ESP32's **own webapp commands**: all solenoids fire, **in sync, only on command**, cleanly.
Pull the Enttec out and the solenoids stop responding.

It works **whether the Enttec is transmitting or just listening** (RX monitor). The only thing
the Enttec changes by its mere presence is the **electrical state of the bus** — and since
drive-vs-listen makes no difference, it is supplying a **ground reference**, not drive strength.

### Root cause

The DMX source is the **M5Stack Unit DMX, which is galvanically isolated** (CA-IS3092W). Its
DMX-side ground **floats**, so nothing anchors the A/B pair's common-mode voltage.

RS-485 receivers only resolve A−B while **both wires sit inside their common-mode input range
(≈ −7 V to +12 V of the receiver's own ground)**. With no reference the common-mode drifts
(leakage, coupling, static) until the decoders' inputs rail — they can no longer read the
difference and **misread the valve channels → solenoids don't fire, or chatter.** The Enttec is
**non-isolated and laptop-grounded**; connecting it — even only to listen — hands the bus the
reference it was missing, so the decoders un-rail and the ESP32's signal reads perfectly.

### Multimeter confirmation

Metered on the M5 Unit DMX: **the DMX-side ground does not connect to the accessible
(logic/USB) ground — it is isolated.** So the M5 **cannot** provide a bus ground reference. This
is not a fault to repair; the isolation is working as designed, and it is exactly the source of
the problem.

### What this overturns

- **Double termination (60 Ω) — WRONG.** Adding a device (the Enttec) *helped*; a bus that
  improves when you add load was **under-referenced, not over-terminated**.
- **Weak transmitter / drive strength — not primary.** The Enttec fixes it while merely
  *listening*, i.e. not driving at all.
- **Firmware — exonerated (again).** The Enttec RX capture at the **end of the chain** showed
  valve channels **1 / 8 / 23 / 38 / 53 all clean `0 → 255 → 0`, no noise** — so the firmware
  commands every valve correctly (`flameLevel = 255`). The "tower solenoids not firing" was the
  missing ground reference, **not** `flameLevel` and **not** DMX corruption.

### Why a "balanced" protocol still needs a ground

Differential signalling rejects common-mode noise **only within the receiver's common-mode input
range**. The ground reference is what keeps both wires inside that window. **Isolated ≠
groundless** — each isolated bus segment still needs its own **local** ground reference (DMX
pin 1). "It's balanced, it doesn't need a ground" is the myth that leaves pin 1 floating and
produces exactly these intermittent, layout-dependent failures.

### The fix — reference from the RECEIVER (fixture) side, since the M5 can't provide it

| # | Fix | Notes |
|---|-----|-------|
| 1 | **Fail-safe bias resistors** referenced to the decoders' ground (~680 Ω A→+5 V, B→GND, with the 120 Ω termination) | Cheap DIY. Anchors common-mode into range — exactly what the Enttec does passively. |
| 2 | Keep a **grounded device** on the bus (the Enttec) | Proves the mechanism; not a field fix (no laptop dangling in the field). |
| 3 | **Grounded, powered opto-isolated DMX splitter / re-driver** | **Recommended.** Output is ground-referenced to the fixtures → provides the reference **and** strong drive **and** per-branch termination. Enables hub-and-spoke (one tower per output). |

> Caveat: a hard ground bond defeats the M5's isolation — fine on a single-supply rig, but if
> towers ever run on **separate mains**, use a grounded re-driver (#3) to avoid ground loops.

### In progress at session end (UNRESOLVED)

Trying a **RioRand DMX amplifier** (an active, powered re-driver = fix #3), wired **inline**
(M5 → amp **IN**, amp **OUT** → fixture chain). **Green LED flashing, but signal is not reaching
the chain.** Suspects, in order:

1. **Data polarity swap** (XLR pin 2 = Data−, pin 3 = Data+) on the amp output — the #1 cause.
2. **IN/OUT reversed** (amplifiers are one-way).
3. **GND terminal not wired** on the amp.
4. **Termination** — need a single 120 Ω at the end of the amp's output chain.

Next: put the **Enttec RX monitor on the amp's OUTPUT** — channels present = amp is driving,
problem is downstream; nothing = amp isn't locking its input. Also drive the amp **INPUT** from
the Enttec to isolate the M5→amp link.

### Untested change — RC-coupled "soft ground" reference (2026-08-04)

Instead of a hard bond, coupled the **M5 logic-side ground to the DMX bus ground through a
330 Ω resistor in parallel with a 3 µF capacitor**. This gives the isolated bus a **soft ground
reference** without a DC short: the resistor bleeds static / sets a current-limited DC
reference, the cap (Xc ≈ 0.2 Ω at 250 kHz) shorts high-frequency common-mode to that reference.
**It fires fine this way — but is NOT fully tested.**

This is a **recognized technique** for referencing an isolated RS-485 node (TI/ADI app notes use
an R‖C from isolated ground to local ground); it just isn't in DMX's pure-isolation model and
isn't built into the bridge we bought.

**Still to verify:**
- Confirm it holds with the **Enttec removed** — this RC is meant to *replace* the Enttec as the
  reference. That's the real pass/fail.
- Test **all towers + a sustained purge hold**, watching for any return of the chatter.
- It **partially bridges the M5 isolation** → fine on a single supply, but if towers ever run on
  **separate mains**, ground-loop / leakage current flows through the RC. Check the **resistor's
  power rating** and the **cap's voltage rating**; prefer the grounded re-driver (#3) for the field.
- **3 µF is large** for this (typical: resistor + a nF-range cap). Works, but non-standard.

### Firmware changes flashed this session (2026-08-05)

- `DMX_FRAME_SLOTS` **128 → 64** — reverted the regression that had killed the accumulator light.
- Frame rate kept **deliberately slow at 20 Hz** (`DMX_FRAME_INTERVAL_MS = 50`).
- Added **`dmxReadyToSend()`** — queries `Serial1.availableForWrite()` and skips a frame if the
  previous one hasn't fully drained, so a break can never land mid-frame.
- Frame **sent/skipped counters** staged in `dmx.cpp`; valve-channel serial logging proposed but
  **not yet flashed**.

### Firmware behaviour confirmed (stop re-checking these)

- **All solenoids fire in sync on button PRESS** (FSM `IDLE → FIRE_ACTIVE` on press). Release only
  closes early in PARTY / MACHINE_GUN; **FIREBALL ignores release** and runs the full `fireDurationMs`.
- **Uplights show the theme colour during fire**; the white is the **END_CUE fade** *after* the fire
  ends. Fire/white independence kept (chosen — no change).
- **Cooldown observed ≈ 50 ms** (effectively disabled) — confirm whether intended.
- The `[DMX]` serial log prints **only CH1–5 and only on FSM state changes** → **blind to the tower
  valves (8/23/38/53) and to purge.** That is why the serial log looked empty about the towers.

### New tooling built

- **`tools/dmx-tester/index.html`** — Web Serial reference console + RX monitor (Enttec Pro
  protocol), with **change-capture logging** that timestamps every valve-channel change at the
  chain end. Produced the clean capture above. Open in a Chromium browser (Edge/Chrome).
- **`docs/spec-field-diagnostics.md`** — field kit + line-vs-component verdict method.

### Next actions

- [ ] Get the RioRand amp passing signal (polarity → IN/OUT → GND → termination); verify with the
  Enttec RX monitor on its output.
- [ ] **Confirm the fix:** amp (or bias resistors) in place, **Enttec removed**, fire from the
  webapp → solenoids respond. That is the win condition.
- [ ] For a permanent, field-portable answer, prefer the **grounded opto splitter (hub-and-spoke)**,
  one tower per output.
- [ ] Decide on fail-safe bias resistors if not using a re-driver.
- [ ] (Optional) flash the valve-channel serial logging + frame counters for field visibility.

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
