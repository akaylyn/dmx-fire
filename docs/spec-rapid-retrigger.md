# Spec: Rapid retrigger

## Context

Tapping the button repeatedly produced roughly **one shot per 1.1 seconds**, no
matter how low `cooldownMs` was set. Setting cooldown to 50 ms changed nothing
perceptible.

`cooldownMs` was not the cause. `button_fsm.cpp` had a **hardcoded 1000 ms** end cue:

```cpp
case FSM_END_CUE:
  if (elapsed >= 1000) enterState(FSM_COOLDOWN);   // literal, not config
```

Every shot runs `FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE`, so the floor was
`fireDurationMs + 1000 + cooldownMs`. Below one second, cooldown was irrelevant —
the end cue dominated. Secondary contributors: the web sliders floored at 50 ms with
50 ms steps, so sub-50 ms values were not even expressible.

Related: [spec-dmx-transmit.md](spec-dmx-transmit.md) (why the bus is slow),
[spec-fire-uplight.md](spec-fire-uplight.md) (companion change),
[spec-machine-gun.md](spec-machine-gun.md), [spec-fire-lockout-confluence.md](spec-fire-lockout-confluence.md).

---

## ⚠️ The hard floor is the DMX bus, not the FSM

`DMX_FRAME_INTERVAL_MS = 50` (20 Hz) in `dmx.h`. That rate is **deliberately slow**
and is a field fix — commit `41404cf`, "restore 64-slot frame, slow refresh" — for
tower-1 flicker and phantom firing on a noisy bus. See
[spec-dmx-transmit.md](spec-dmx-transmit.md) lines 83-85 and
[dmx-isolated-grounding.md](dmx-isolated-grounding.md).

A valve byte can therefore only change **once per 50 ms frame**:

| | Duration |
|---|---|
| Shortest expressible valve-open pulse | 50 ms (1 frame) |
| Shortest expressible gap between pulses | 50 ms (1 frame) |
| **Shortest achievable shot cycle** | **~100 ms ≈ 10 shots/sec** |

That is roughly **10× faster than the 1.1 s** it was, but it is not 10 ms. Tapping
faster than ~100 ms cannot be represented on this bus at 20 Hz. **`dmx.h` is not
modified by this change** — speeding the bus up would reverse the flicker fix.

There is also a mechanical floor below the electrical one: a propane solenoid takes
real time to open and close, so pulses near the frame floor may not produce a
distinct fireball even when the byte is correct on the wire.

---

## Changes

### 1. `endCueMs` — the end cue is configurable and skippable

New `uint16_t endCueMs` in `ButtonConfig`, default `1000`, NVS key `btncuems`.

`FSM_END_CUE` now exits on `elapsed >= buttonConfig.endCueMs`. When `endCueMs == 0`
the state is **skipped entirely** rather than passed through for one tick:

```cpp
static FsmState afterFireState() {
  return (buttonConfig.endCueMs == 0) ? FSM_COOLDOWN : FSM_END_CUE;
}
```

Both `FIRE_ACTIVE` exits — duration expiry and the mode 1/2 release — go through it.
Skipping rather than zero-length passing means `/api/state` never shows a one-frame
`END_CUE` blip that would confuse the pytest state waits.

The white-flash fade in the main loop now scales to `endCueMs` instead of the literal
`1000`, and is guarded so it can never divide by zero.

### 2. One guaranteed frame of valve-open

**Required for short durations to work at all.** The DMX block samples `fsmState`
once per 50 ms frame. With `fireDurationMs` at 10–40 ms the entire `FIRE_ACTIVE`
window can fall *between* two frames — the FSM enters and leaves it without a single
frame ever observing it, so a fast tap commands nothing and no fire happens.

A sticky latch is set on every entry to `FIRE_ACTIVE` and drained once per frame:

```cpp
bool fsmConsumeFirePending();   // true if FIRE_ACTIVE was entered since last call
```

```cpp
bool firePending = fsmConsumeFirePending();
bool firing = (fsmState == FSM_FIRE_ACTIVE) || firePending;
```

**Call it exactly once per frame, before the tower loop.** It drains state, so a
second call in the same frame returns false — calling it inside the loop would give
tower 0 fire and towers 1-3 nothing. Computing `firing` once up front also keeps all
five valves (four towers plus Confluence) agreeing on the same frame.

> ⚠️ **Do not collapse this to one line.** Written as
> `bool firing = (fsmState == FSM_FIRE_ACTIVE) || fsmConsumeFirePending();`, `||`
> short-circuits: during a normal burn the left side is true every frame, so the
> latch is never consumed. It then survives until after the FSM leaves
> `FIRE_ACTIVE` and fires a **spurious extra frame of valve-open** — propane after
> the FSM said stop, and the end-cue fade suppressed for that frame. The latch must
> be drained unconditionally.

`buttonInjectReset()` clears the latch, so a reset never leaves a frame of
valve-open owed.

### 3. Machine gun pulses the tower valves too

Previously only Confluence pulsed; the tower valves sat flat open for the whole burn
while the centre stuttered. The pulse gate is now computed once above the tower loop
and shared:

```cpp
bool mgOn = true;
if (buttonConfig.mode == 2) {
  uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + DMX_FRAME_INTERVAL_MS;
  mgOn = (millis() % period) < (uint32_t)buttonConfig.machineGunBurstMs;
}
```

Towers use `state.fireOpen = mgOn && fireEnabled`; Confluence uses the same `mgOn`.
The off-time was already a hardcoded 50 ms — exactly one DMX frame, the floor — so
it is now written as `DMX_FRAME_INTERVAL_MS` for clarity. No behaviour change there.

The **uplight does not pulse** with the valve; see
[spec-fire-uplight.md](spec-fire-uplight.md).

---

## Web UI

| Control | Was | Now |
|---|---|---|
| Fire duration (ms) | 50–10000 step 50 | **10**–10000 step **10** |
| Cooldown (ms) | 50–30000 step 50 | **0**–30000 step **10** |
| Machine gun burst (ms) | 50–2000 step 50 | **10**–2000 step **10** |
| End cue | single-option `<select>` | **slider** 0–2000 step 10, default 1000 |

The dead `endCuePattern` `<select>` — one option, "White flash fade", with the
documented "colour cascade" variant never implemented — is replaced by the `endCueMs`
slider. **The `endCuePattern` field itself is kept** in `ButtonConfig`, NVS and
`/api/state`; `tests/test_config_button.py` asserts on it, and `handleSet()` only
overwrites it when a client actually sends it.

A hint under the panel states the 50 ms frame floor and that end cue 0 is what
unlocks rapid retrigger.

**Rapid-fire preset:** mode Fireball, fire duration 50 ms, end cue 0, cooldown 0.

### `/api/state`

`button.endCueMs` added.

---

## Persistence

One new global NVS key: `btncuems` (`uint16_t`, default 1000). Existing devices pick
up the 1000 ms default on load, so behaviour is unchanged until the operator lowers
it — no `--erase` needed.

---

## Tests

New in `tests/test_fsm_transitions.py`:

- **`test_end_cue_is_configurable`** — `endCueMs=200` shortens the state.
- **`test_end_cue_zero_skips_the_state`** — polls throughout and asserts `END_CUE` is
  never observed while `COOLDOWN` is.
- **`test_rapid_retrigger_cycle_is_fast`** — a full press→release→IDLE cycle with
  end cue 0 and cooldown 0 completes in under 1 s (was ~1.1 s).
- **`test_short_tap_still_opens_a_valve`** — fires sub-frame taps and asserts a valve
  byte reaches the wire. Direct guard for the one-frame latch.

New in `tests/test_dmx_output.py`:

- **`test_machine_gun_pulses_tower_valves`** — both the tower valve and Confluence
  are observed at 0 *and* at their fire level during a burst.

New in `tests/test_config_button.py`: **`test_button_end_cue_ms_round_trip`**,
**`test_button_fast_values_accepted`**.

`tests/conftest.py` pins `endCueMs=1000` in the baseline so the existing timing tests
see the same END_CUE they always did.

---

## Non-goals

- **No change to the DMX refresh rate.** `dmx.h` stays at 20 Hz. The ~100 ms shot
  floor is accepted. Reversing `41404cf` to chase 10 ms taps risks reintroducing the
  flicker and phantom firing it fixed — that trade is not worth it for shot rate.
- **No removal of `endCuePattern`.** The dead "colour cascade" field stays as-is;
  removing it is a separate cleanup.
- **No cooldown safety floor.** `cooldownMs` can now be 0. The lockout exists to stop
  rapid solenoid re-fire, and setting it to 0 is a deliberate operator choice —
  `fireDurationMs` and the 50 ms frame floor still bound how fast propane can flow.
- **No auto-tuning.** Nothing clamps `fireDurationMs` up to the frame interval; a
  10 ms setting is honoured as "one frame" by the latch rather than being rewritten.
- **No change to Morse.** It still drives Confluence CH1 only and does not gate on
  `fsmState`.
