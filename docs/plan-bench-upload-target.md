# Plan: bench upload target (M5CoreS3)

> **Status: not started.** Saved for later — nothing in this document has been
> implemented. Written when no spare AtomS3 was available for testing.
> Follows the format of [plan-web-config.md](plan-web-config.md).

## Context

Two upload changes are sitting in the working tree **completely untested on
hardware**, because no device was available when they were written:

- **OTA over WiFi** (`ota.cpp`, `scripts/ota.sh`, Firmware tab) — the new primary
  upload path. `Update.begin/write/end`, the OTA slot roll and the reboot have
  never run.
- **A rewritten USB flash strategy** (`flash.sh`) that holds the chip in download
  mode across blocks instead of resetting into the app 18 times. Measured
  motivation: a real flash spent **461 s wall clock on 8.8 s of data transfer**
  (~2% efficiency) and then failed on the final block, needing a physical power
  cycle to recover.

On top of that, the whole fire-uplight + rapid-retrigger feature set (49 pytest
tests, 12 of them new) has never been run against a device.

There is no spare AtomS3, but there is an **M5CoreS3**. It is a valid test
vehicle: it uses the ESP32-S3's **native USB-Serial/JTAG** (confirmed — not a
CH9102 bridge), so it exercises the exact interface that wedges. It has no DMX
wiring and no GPIO39 button.

**Two things this unlocks beyond upload testing:**

1. Every pytest DMX assertion reads the **shadow buffer over HTTP**
   (`/api/state` → `dmx.ch`), not real light output, and every test drives the
   FSM through API injection rather than the physical button. So the full suite
   runs on a CoreS3 with nothing attached — finally validating the uplight and
   retrigger work.
2. The flash-strategy hypothesis is *"the app rebooting between blocks wedges
   USB."* Testing it needs firmware that is **USB-hostile in the same way** —
   booting, starting the WiFi AP, running the DMX loop. A stripped-down build
   would pass in legacy mode too and prove nothing. Hence: the bench build keeps
   DMX running into an unconnected Port A.

**The CoreS3 is bench scaffolding, not a deployment target**, and is to be
removable in one commit once a spare AtomS3 exists.

---

## Starting state: partially-applied work to clean up

An earlier attempt at this was interrupted part-way, so execution begins from a
dirty tree. Reconcile before anything else:

| Item | Action |
|---|---|
| `Test_Button_DMX/board_config.h` (untracked) | **Rename → `board_hal.h`** and reshape into the HAL surface; the pin macros carry over as-is |
| `.ino` `#if HAS_STATUS_LED` / `#if HAS_PHYSICAL_BUTTON` guards (~6 sites) | **Revert** — replaced by unconditional HAL calls |
| `.ino:217` unguarded `M5.BtnA` | **Fix** — was missed entirely (see below) |
| `dmx.cpp` using `DMX_RX_PIN`/`DMX_TX_PIN` | **Keep**, just re-point the include |
| `flash.sh` FQBN parameterisation | **Keep** — already correct |
| `ota.sh`, `flash-progress.sh` | **Not applied** — those edits were rejected |

Also uncommitted and unrelated to this plan: the fire-uplight/rapid-retrigger
feature work and the OTA/flash changes themselves. This plan does not touch fire
behaviour; if the CoreS3 run passes, it retroactively validates that work too.

---

## Design

### One firmware, one file of conditionals

The production sketch must contain **no `#if` board guards**. All board
divergence lives behind small functions in a single new module, which the sketch
calls unconditionally.

> **Deviation worth stating:** this is a `.h`/`.cpp` pair, not a lone header. The
> real implementations own file-scope state (`CRGB ATOM_LED[1]`,
> `m5::Button_Class keyButton`, the `FastLED.addLeds` registration). Defining
> those in a header included by more than one translation unit — `dmx.cpp` needs
> the pin macros — gives duplicate definitions. A `.cpp` also matches every other
> module here (`dmx`, `towers`, `themes`, `confluence`, `button_fsm`, `morse`,
> `storage`, `web`, `ota`).

**New: `Test_Button_DMX/board_hal.h` / `board_hal.cpp`**

```cpp
// board_hal.h — pin macros + the HAL surface. The ONLY place that knows
// which board this is.
#define BOARD_NAME  ...        // reported in /api/state and the boot banner
#define DMX_RX_PIN  1          // Port A, same on both boards
#define DMX_TX_PIN  2

void boardSetup();                       // pins, LED init, button init
void boardButtonPoll();                  // sample the physical button, if any
bool boardButtonPressed();               // always false with no button
bool boardButtonReleased();
bool boardButtonHeld();
void boardStatusLedTick(FsmState s);     // throttling + FastLED.show() inside
bool boardDiagnosticsRequested();        // M5.BtnA on the rig; false on bench
```

`board_hal.cpp` carries `#if defined(ARDUINO_M5STACK_CORES3)` (bench: every
function a no-op / `false`) versus the default AtomS3 branch (GPIO39 button,
WS2812 on GPIO35). The bench branch emits a `#warning` so every bench compile
announces itself.

**Board macros** come from `build.board` in boards.txt, verified:
`m5stack_atoms3` → `ARDUINO_M5STACK_ATOMS3`, `m5stack_cores3` →
`ARDUINO_M5STACK_CORES3`.

**Not a PSRAM problem:** CoreS3 uses **QSPI** PSRAM, so GPIO35 is not reserved —
the WS2812 pin is skipped because there is no LED there, not to avoid a conflict.

**Partitions are fine on both.** Each board resolves `build.partitions=default`
against its own flash size (AtomS3 8 MB → `app0`/`app1` 3264 K each at
`0x10000`/`0x340000`; CoreS3 16 MB → larger). Both therefore have the two OTA
slots plus `otadata` that `Update.h` needs, and the 1.1 MB image fits either with
room to spare. No partition work is required. The CoreS3 binary will be a
slightly different size, which is precisely why `flash-progress.sh` must derive
its block total from the selected build rather than a hardcoded path.

### What the bench build changes vs the rig

| | AtomS3 (rig) | CoreS3 (bench) |
|---|---|---|
| Status LED (WS2812 GPIO35) | yes | **skipped** — no such LED |
| Physical button (GPIO39) | yes | **skipped** — floating LOW would look like a held button and fire on repeat |
| `M5.BtnA` → run diagnostics | yes | skipped |
| DMX on Port A @ 20 Hz | yes | **yes, into nothing** — keeps USB load realistic |
| WiFi AP, web server, OTA, FSM, themes | yes | yes |

### Sketch changes (`Test_Button_DMX.ino`)

Replace the interrupted `#if` guards with straight calls.

- `setup()`: `boardSetup();` replaces the `pinMode`/`FastLED.addLeds` block.
- `loop()`: `boardButtonPoll();` then OR the three `boardButton*()` results with
  the existing `buttonConsumePress()` / `buttonConsumeRelease()` /
  `buttonVirtualHeld()` — the API path is unchanged and stays the only FSM
  source on the bench.
- `boardStatusLedTick(fsmState);` replaces the LED `switch` and the bare
  `FastLED.show()`.
- `if (boardDiagnosticsRequested()) runDiagnostics();` — **this one is a real bug
  in the interrupted work, not just tidiness.** `.ino:217`'s
  `M5.BtnA.wasPressed()` was left unguarded; on CoreS3 M5Unified maps `BtnA` to a
  touchscreen region, so a stray touch re-runs `runDiagnostics()` — which drives
  a 500 ms full-white DMX visual test mid-session.

`M5.Ex_I2C.release()` (`.ino:54`) stays unconditional for now. It frees Port A
for Serial1 on the AtomS3; CoreS3's Port A is also G1/G2 and its on-board I2C
(AXP2101 PMIC, touch) is reached via `M5.In_I2C`, so releasing the external bus
should be harmless — but it is an AtomS3-derived assumption. If the CoreS3 fails
to boot or the PMIC misbehaves, this is the first thing to route through the HAL.

`dmx.cpp` takes `DMX_RX_PIN` / `DMX_TX_PIN` from `board_hal.h`.

FastLED stays unconditionally included — `themes.cpp` and `towers.h` need
`CRGB`/`CRGBPalette256` regardless. Only the LED *driver* use is skipped.

### `/api/state` reports the board

Add `"board":"<BOARD_NAME>"` alongside `boot_id`. Currently `BOARD_NAME` is only
printed to serial at boot and is **not exposed over HTTP at all**.

This is not cosmetic. Both boards bring up the *same* AP SSID from `secrets.h` at
the *same* `192.168.4.1`, and nothing in the repo distinguishes them. With both
powered on, `scripts/ota.sh` or the pytest suite could silently talk to the wrong
device — including pushing a CoreS3 build at the real rig. With a `board` field,
`ota.sh` and the tests can assert which target they reached.

> **Operator rule while the bench target exists: never power both boards at
> once.** The `board` field turns a silent mistake into a loud one; it does not
> prevent the collision.

---

## Scripts: target selection

Everything build-related takes the FQBN from one env var, defaulting to the rig
so existing invocations are unchanged:

```bash
FQBN="${DMXFIRE_FQBN:-m5stack:esp32:m5stack_atoms3}"
BUILD="$SKETCH/build/${FQBN//:/.}"     # arduino-cli replaces ':' with '.'
```

Current state, from a full inventory:

| File | Status |
|---|---|
| `tests/visual/scripts/flash.sh:72-74` | **done** — `DMXFIRE_FQBN` + derived `FQBN_DIR` |
| `scripts/flash.sh` | **free** — 9-line `exec` wrapper, env passes through |
| `scripts/ota.sh:20,22` | **to do** — FQBN and build dir both hardcoded |
| `scripts/flash-progress.sh:35` | **to do** — hardcoded build path is the progress denominator |

Left unfixed, `flash-progress.sh` falls back to a hardcoded 18 blocks for a
CoreS3 build of a different size: the verified count stays right, the percentage
goes wrong.

`--chip esp32s3` is hardcoded at `flash.sh:238,272`. Fine for both boards here
(both are S3); it would need attention only for a non-S3 target, which is a
non-goal.

### Dangling reference to fix

`board_config.h:8` and `flash.sh:71` **already cite `docs/spec-upload-targets.md`,
which does not exist.** Writing it (below) closes that.

Usage:

```bash
DMXFIRE_FQBN=m5stack:esp32:m5stack_cores3 scripts/flash.sh
DMXFIRE_FQBN=m5stack:esp32:m5stack_cores3 scripts/ota.sh
```

---

## Verification

The point of the exercise. Run in this order, on the CoreS3:

**1. Both boards still compile**
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX
arduino-cli compile --fqbn m5stack:esp32:m5stack_cores3 Test_Button_DMX
```
The rig binary must be byte-identical in behaviour to before — the HAL is a
refactor, not a change.

**2. USB flash, new strategy** — the headline test. `flash.sh` tees its own log
under `tests/visual/runs/` and `flash-progress.sh` auto-detects it, so no `tee`
plumbing is needed:
```bash
DMXFIRE_FQBN=m5stack:esp32:m5stack_cores3 scripts/flash.sh
scripts/flash-progress.sh                   # separate terminal
```
Record wall clock and retry count. Expectation: well under the 461 s baseline,
with few or no retries.

**3. USB flash, legacy strategy** — the control.
```bash
DMXFIRE_FQBN=m5stack:esp32:m5stack_cores3 scripts/flash.sh --legacy
```
Gives a same-hardware baseline. If legacy is slow/flaky and the new path is fast
and clean, the hypothesis holds. **If both are clean, the test is inconclusive**
— the CoreS3 may simply not exhibit the AtomS3's wedging, and the AtomS3 remains
unproven. Say so rather than claiming a fix.

**4. OTA** — first real exercise of `Update.*`.
```bash
DMXFIRE_FQBN=m5stack:esp32:m5stack_cores3 scripts/ota.sh
```
Then again from the web UI's Firmware tab to test the browser path and progress
bar. Confirm the device reboots with a fresh `boot_id`.

**5. Full pytest suite** — validates the untested feature work.
```bash
scripts/test.sh --api        # --api ONLY
```
All 49 should pass on the CoreS3, including the 12 new uplight/retrigger tests,
because they assert on the DMX shadow buffer over HTTP and drive the FSM by API
injection.

> **Do not run `--visual` or `--all` on the bench board.** That harness
> (`tests/visual/scripts/visual_test.py`) photographs real fixtures with
> `imagesnap`; with nothing wired it is meaningless and will fail.

Expect cosmetic `[FAIL]` lines on the bench board's *serial* boot diagnostics
(`tests.cpp:32-61` flags towers/confluence as disconnected). Informational only —
it does not abort, and it does not affect pytest.

**6. Safety re-check on the bench:** confirm `/api/state` shows `fsm=IDLE` at
rest and never enters `FIRE_ACTIVE` on its own — proof the absent GPIO39 button
is not floating into a phantom press.

---

## Docs

- **`docs/spec-upload-targets.md`** (new) — closes the dangling reference from
  `board_config.h:8` and `flash.sh:71`. Covers why a bench target exists, the
  HAL, the rig-vs-bench table, the same-SSID warning, and a **"How to remove
  this"** section listing exactly what to delete when a spare AtomS3 arrives.
- `docs/spec-ota-update.md` and `docs/spec-upload.md` — replace the "unverified
  on hardware" warnings with whatever the CoreS3 run actually shows, including
  the numbers from steps 2 and 3.
- `CLAUDE.md` — the `DMXFIRE_FQBN` override, and a short note that CoreS3 is
  bench-only.
- **`.claude/skills/upload/SKILL.md`** — it hardcodes an absolute path with no
  way to pass `DMXFIRE_FQBN`, so `/upload` can only ever hit the rig. It is also
  already stale: `CLAUDE.md` now calls OTA the preferred path and the skill never
  mentions `scripts/ota.sh`. Add the OTA path and a bench-target invocation.

Docs carrying a hardcoded FQBN that should gain a note (or the env form):
`CLAUDE.md:9`, `README.md:44`, `.claude/skills/web-sync/SKILL.md:119`,
`docs/spec-upload.md:87`, `docs/spec-api-button-and-tests.md:168`,
`docs/spec-per-tower-control.md:163`. **Leave `docs/manuals/m5atomS3-lite.md`
alone** — it is the AtomS3 datasheet, where the literal FQBN is correct.

---

## Non-goals

- **CoreS3 is not a deployment target.** No DMX wiring, no button, no fire
  hardware. It exists to exercise upload paths and the HTTP API.
- **No CoreS3-specific features.** No LCD UI, no touch input, no use of anything
  the CoreS3 has and the AtomS3 does not.
- **No third board.** The HAL has exactly two branches; generalising further is
  speculative.
- **No change to the DMX refresh rate**, the 64 KB block size, or any fire
  behaviour. This work is target plumbing only — `DMXFIRE_BLOCK_SIZE=262144`
  stays available but untried so a field failure has one variable, not two.
- **A passing CoreS3 flash does not prove the AtomS3 is fixed.** Different board,
  possibly different silicon revision. It raises confidence; it does not close
  the question.
