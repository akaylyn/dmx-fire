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
| Behaviour | While button held (or until `fireDurationMs` cap), the solenoid pulses ON for `machineGunBurstMs`, then OFF for 50 ms, repeating |
| Stops on release | Yes (same as Party mode 1) |
| FSM path | Standard `IDLE → FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE` |
| Cooldown | Same `cooldownMs` as other modes |

### New `ButtonConfig` field

```c
uint16_t machineGunBurstMs;   // solenoid on-time per pulse, default 200 ms
```

Range in UI: 50–2000 ms in 50 ms steps. The 50 ms floor matches the minimum the solenoid can reliably open/close cleanly.

### Pulsing implementation

In the 50 Hz DMX tick (`Test_Button_DMX.ino`), when `fsmState == FSM_FIRE_ACTIVE` and `buttonConfig.mode == 2`:

```c
uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + 50;
cfLevel = (millis() % period < (uint32_t)buttonConfig.machineGunBurstMs)
          ? confluenceConfig.fireLevel : 0;
```

Non-blocking — `millis() % period` produces a square wave with `machineGunBurstMs` ON, 50 ms OFF, indefinitely while fire is active.

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

The user lowered the minimum `cooldownMs` from 2000 ms to 50 ms specifically to make machine-gun-mode fast cycles practical. A 50 ms cooldown means you can re-trigger immediately after the END_CUE settles, supporting rapid trigger-release-trigger sequences without the FSM forcing a long pause.

---

## Non-goals

- **Programmable burst pattern.** Only uniform period. Variable patterns (e.g. "burst burst pause burst") belong in the [Morse code feature](spec-morse-code.md), which is more general.
- **Velocity / intensity ramp.** Burst level is always `confluenceConfig.fireLevel`. The slider gates time, not flame intensity.
