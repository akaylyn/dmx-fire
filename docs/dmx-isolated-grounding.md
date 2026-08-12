# Reference: Isolated DMX (M5 Unit DMX / ESP32) — Grounding & Large Chains

## Why this doc exists

During field debugging (see [../notes.md](../notes.md) "Session 3", 2026-08-04) the rig's
solenoids intermittently failed, chattered, or didn't respond at all. The root cause was **not**
termination, drive strength, or firmware — it was a **missing bus ground reference**: the
M5Stack Unit DMX is galvanically isolated, so the DMX bus had no ground reference and the
decoders' RS-485 receivers drifted out of common-mode range and misread the valve channels.

This doc captures the general principle, why it isn't specific to the M5, how large DMX chains
are run reliably, and the authoritative sources — so it never has to be re-derived on a dusty
field call. It pairs with [spec-field-diagnostics.md](spec-field-diagnostics.md).

## It's a known DMX problem, not an M5/ESP32 quirk

The symptom — **one end of the DMX line references earth, the other is isolated/floating, so the
reference drifts and you get "no control or flicker"** — is a standard DMX troubleshooting entry.
The pro fix prescribed in the guides is exactly what worked on our rig:

1. **Lift pin 1 (common) at one end** of the DMX line.
2. If that helps, **add an optical isolator inline** to fix it properly rather than leaving a
   floating reference.

So the rig walked straight into a documented failure mode; nothing exotic.

## Why the M5 Unit DMX is prone to it

- It uses the **CA-IS3092W isolated half-duplex RS-485 transceiver (5 kVrms)**. The isolation
  protects the ESP32 from ground faults, but an isolated node's **bus-side ground must be
  referenced somewhere**, and the module leaves that to the integrator.
- Contrast with cheap **non-isolated MAX485** ESP32 rigs: those are hard-grounded through the
  ESP32's own supply, so they never float — but they instead risk ground loops and fault damage.
  The M5 trades one problem for the other and does not document the consequence.
- Note: the M5's **120 Ω terminator is an onboard switch** (switchable), per the M5 docs — so it
  is not a fixed built-in as an earlier field note assumed.

## Why "balanced" still needs a ground reference

RS-485 is differential, but the receiver only resolves A−B while **both wires sit inside its
common-mode input range (≈ −7 V to +12 V of the receiver's own ground)**. With an isolated
source and no reference, parasitic charging pushes the common-mode outside that window and the
receiver rails. **Isolated ≠ groundless** — each isolated segment still needs its own *local*
ground reference (DMX pin 1). "It's balanced, it doesn't need a ground" is the myth that leaves
pin 1 floating and produces intermittent, layout-dependent failures.

## The referenced-isolation fix (what we used on the bench)

The textbook method for referencing an isolated node is **resistor(s) to bleed static charge to
ground + a capacitor to absorb charge and limit peak voltage** across the barrier. Our bench
fix was a **330 Ω ‖ 3 µF** network from the M5 logic ground to the DMX bus ground. It fires fine
but is not fully field-tested — see the caveats in notes.md (it partially bridges the isolation;
watch the resistor's power rating and the cap's voltage rating if towers ever run on separate
mains).

## Running large chains reliably (documented practice)

Nobody runs a big isolated rig off one weak, unreferenced source. The standard rules:

- **≤ 32 unit loads per segment** (RS-485 hard limit). eldoLED recommends a **booster roughly
  every 25 fixtures**.
- **Opto-isolated splitters / boosters** at branch points — each output **separately powered and
  ground-referenced to its own fixtures** (this is the hub-and-spoke topology). "The more you
  split, the easier it is to isolate and identify problems."
- **120 Ω termination at the last device** of each branch; **twisted-pair, 120 Ω,
  low-capacitance** cable.
- **Controller bonds mains earth to the DMX network** as the reference point; **shield grounded
  at one end only** (the controller).

**Takeaway:** don't ask the isolated ESP32 to reference the whole bus. Put a properly-grounded
re-driver between it and the fixtures, and split into short, terminated, referenced segments.

## Sources

- **Isolated RS-485 grounding / the R‖C reference network:**
  - [EDN — Inside an isolated RS-485 transceiver](https://www.edn.com/inside-an-isolated-rs-485-transceiver/)
  - [Analog Devices — Isolated RS-485 Transceiver Breaks Ground Loops](https://www.analog.com/en/resources/technical-articles/isolated-rs485-transceiver-breaks-ground-loops.html)
- **DMX-specific "isolated end floats → lift pin 1 → add optical isolator":**
  - [ETC — DMX512 Info](https://support.etcconnect.com/ETC/Getting_Started_with_ETC_and_FAQ/DMX512-Info)
  - [DMXDesktop — Troubleshooting](https://www.dmxdesktop.com/troubleshooting)
- **Large-install grounding / unit-load / splitter practice:**
  - [ESTA — Recommended Practice for DMX512 (USITT DMX512-A guide)](https://tsp.esta.org/tsp/documents/docs/DMX512-A_Guide_(8x10)_ESTA.PDF)
  - [Pathway/Acuity — DMX Installation Misconceptions](https://pathway.acuitybrands.com/dmx-512-installation-misconceptions)
  - [eldoLED — How to Wire a DMX System](https://eldoled.com/insights/how-to-wire-dmx-lighting-systems/)
- **The M5 module (CA-IS3092W, switchable terminator):**
  - [M5Stack Unit DMX docs](https://docs.m5stack.com/en/unit/Unit-DMX)
