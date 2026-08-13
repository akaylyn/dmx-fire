# Spec: Machine Gun Fire Mode

## Context

The original FSM had two button modes: **Fireball** (mode 0, runs the full `fireDurationMs`) and **Party** (mode 1, fires while held, stops on release). Both open the propane solenoid (Confluence CH1) continuously for the duration of fire.

For staccato effects — short rapid bursts that read more like punctuation than a sustained jet — neither mode works. This spec adds a third mode that pulses the solenoid on/off while the button is held.

---

## Mode 2 — Machine Gun

| Property | Value |
|---|---|
| `buttonConfig.mode` | `2` |
| Triggered by | Physical button press OR `POST /api/button/press` |
| Behaviour | While button held (or until `fireDurationMs` cap), **every valve** pulses ON for `machineGunBurstMs`, then OFF for 50 ms, repeating |
| Stops on release | Yes (same as Party mode 1) |
| FSM path | Standard `IDLE → FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE` |
| Cooldown | Same `cooldownMs` as other modes |

### New `ButtonConfig` field

```c
uint16_t machineGunBurstMs;   // solenoid on-time per pulse, default 200 ms
```

Range in UI: **10**–2000 ms in **10 ms** steps (was 50/50 — see [spec-rapid-retrigger.md](spec-rapid-retrigger.md)). Note the 50 ms DMX frame interval is the real floor: a burst shorter than one frame cannot be expressed on the wire.

### Pulsing implementation

In the 20 Hz DMX tick (`Test_Button_DMX.ino`), the gate is computed **once per frame, above the tower loop**, so every valve pulses in lockstep:

```c
bool mgOn = true;
if (buttonConfig.mode == 2) {
  uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + DMX_FRAME_INTERVAL_MS;
  mgOn = (millis() % period) < (uint32_t)buttonConfig.machineGunBurstMs;
}
```

Towers use `state.fire = mgOn ? towerConfigs[i].flameLevel : 0`; Confluence uses `cfLevel = mgOn ? confluenceConfig.fireLevel : 0`.

Non-blocking — `millis() % period` produces a square wave with `machineGunBurstMs` ON and one DMX frame OFF, indefinitely while fire is active.

> **Updated: tower valves pulse too.** This spec originally described Confluence CH1
> alone. The four tower valves (decoder CH4 at DMX 8/23/38/53) sat flat open for the
> whole burn while only the centre stuttered. All five now pulse together. The
> **uplights do not strobe** with the pulse — they hold the fire look steady for the
> whole burn; see [spec-fire-uplight.md](spec-fire-uplight.md).

---

## Web UI changes (`web.cpp`)

Button Config fieldset:

- **Mode select** gains a `Machine Gun` option (`value="2"`).
- **Machine gun burst (ms)** slider appears (`<div id="mgRow">`) only when mode is 2. The slider is always present in the form; JS toggles its visibility on the `change` event of the mode `<select>`.

### Form submission

`POST /set` with `target=button` now reads/writes `machineGunBurstMs` alongside the existing fields.

### `/api/state` JSON

The `button` object includes `"machineGunBurstMs": <int>` for test assertions.

---

## Persistence

Stored under NVS key `"btnmgburst"` (uint16). Loaded at boot in `storageLoad()`, written on every config change via `storageSave()`.

---

## Cooldown interaction

The user lowered the minimum `cooldownMs` from 2000 ms to 50 ms specifically to make machine-gun-mode fast cycles practical, and it now floors at 0.

> **That was not sufficient on its own.** A hardcoded 1000 ms `END_CUE` sat between `FIRE_ACTIVE` and `COOLDOWN`, so lowering cooldown below one second changed nothing — the shot cycle stayed at ~1.1 s regardless. The end cue is now configurable via `endCueMs` and skipped entirely at 0. See [spec-rapid-retrigger.md](spec-rapid-retrigger.md).

---

## Non-goals

- **Programmable burst pattern.** Only uniform period. Variable patterns (e.g. "burst burst pause burst") belong in the [Morse code feature](spec-morse-code.md), which is more general.
- **Velocity / intensity ramp.** Burst level is always `confluenceConfig.fireLevel`. The slider gates time, not flame intensity.
