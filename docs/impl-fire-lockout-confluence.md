# Implementation: Fire Lockout, Button Modes, Confluence & Config

Implements phases 1–4 of `spec-fire-lockout-confluence.md`.

---

## New files

### `confluence.h` / `confluence.cpp`

Owns the `ConfluenceConfig` struct (`connected`, `fireLevel`) and `confluenceWrite(level)`. Writes to DMX ch 1–4; only ch 4 opens the solenoid. CH1–3 are zero-filled (wired but ignored by the fixture).

Defaults: `connected=true`, `fireLevel=255`.

### `button_fsm.h` / `button_fsm.cpp`

Four-state FSM: `FSM_IDLE → FSM_FIRE_ACTIVE → FSM_END_CUE → FSM_COOLDOWN`.

- `buttonFsmSetup()` — initialises `ButtonConfig` defaults (FIREBALL mode, 3 s fire, 10 s cooldown)
- `buttonFsmTick(wasPressed, wasReleased, isHeld)` — call once per `loop()` iteration with one-shot button flags
- `fsmElapsedMs()` — milliseconds since last state transition; used by the main loop for END_CUE fade calculation

**FIREBALL mode**: solenoid runs for the full `fireDurationMs` regardless of release — then locks out.
**PARTY mode**: solenoid closes on release (as well as on timer expiry).

Presses during `FSM_COOLDOWN` are silently ignored.

### `storage.h` / `storage.cpp`

Wraps the ESP32 `Preferences` library (NVS). Namespace: `"dmxfire"`.

`storageLoad()` — call in `setup()` before `webSetup()` so the UI reflects persisted state.
`storageSave()` — called by `handleSet()` after every web config change.

Keys (all ≤ 15 chars):

| Key | Value |
|-----|-------|
| `t0c` … `t3c` | tower connected (bool) |
| `t0b` … `t3b` | tower brightness (uint8) |
| `t0f` … `t3f` | tower flameLevel (uint8) |
| `t0p` … `t3p` | tower palName (String) |
| `cfcon` | confluence connected (bool) |
| `cffl` | confluence fireLevel (uint8) |
| `btnmode` | button mode (uint8) |
| `btnfire` | fireDurationMs (uint16) |
| `btncool` | cooldownMs (uint16) |
| `btncue` | endCuePattern (uint8) |

---

## Modified files

### `towers.h`

Added `flameLevel` field to `TowerConfig`:
```cpp
uint8_t flameLevel;  // 0=off, 255=full open; written to decoder CH4 (W) during fire
```

### `towers.cpp`

- `towerSetup()` initialises `flameLevel = 0` per tower (safe default — no fire until configured)
- `towerWrite()` base address shifted: `const uint16_t base = 4 + index * CHANNELS_PER_TOWER`
  This places Tower 0 at ch 5–19, Tower 1 at ch 20–34, etc., leaving ch 1–4 for Confluence

### `web.cpp`

New sections and behaviours:

| Section | `target` value | New fields |
|---------|---------------|------------|
| Confluence | `"confluence"` | connected, fireLevel |
| All Towers | `"all"` | flameLevel (added) |
| Per Tower | `"0"`…`"3"` | flameLevel (added) |
| Button Config | `"button"` | mode, fireDurationMs, cooldownMs, endCuePattern |

`handleSet()` now calls `storageSave()` after every config change and returns `200` (empty body) instead of `303`. The `brightnesSlider` helper was replaced with the generic `rangeSlider(label, name, value, lo, hi, step)` that handles both byte (0–255) and uint16_t (500–10000 ms) ranges.

Auto-save JS attaches a `change` listener to every `select`, `input[type=range]`, and `input[type=checkbox]` inside each `<form>`. The submit button also triggers a `fetch()` POST and `preventDefault()` — the page never reloads.

### `Test_Button_DMX.ino`

`setup()` call order:
```
dmxSetup() → towerSetup() → confluenceSetup() → buttonFsmSetup() → storageLoad() → webSetup()
```
`storageLoad()` after `*Setup()` calls so it overwrites in-code defaults with persisted values.

`loop()` DMX block (50 Hz) is now FSM-driven:

| FSM state | Tower DMX | Confluence CH4 |
|-----------|-----------|----------------|
| `FSM_IDLE` / `FSM_COOLDOWN` | idle palette + `bright` → CH1–3, `bright` → CH4 | 0 |
| `FSM_FIRE_ACTIVE` | fire palette (`firepal`) → CH1–3, `flameLevel` → CH4 | `fireLevel` |
| `FSM_END_CUE` | 0 → CH1–3, linear fade 255→0 over 1 s → CH4 | 0 |

Onboard ATOM LED reflects state: rainbow (idle) / red (fire) / white (end cue) / amber (cooldown).

---

## DMX universe map (post-implementation)

```
ch  1– 4   Confluence  (ch 4 = solenoid; ch 1–3 written as 0)
ch  5– 8   Tower 0 decoder  (R, G, B, W/flame)
ch  9–19   Tower 0 strobe   (11ch LaluceNatz mode)
ch 20–23   Tower 1 decoder
ch 24–34   Tower 1 strobe
ch 35–38   Tower 2 decoder
ch 39–49   Tower 2 strobe
ch 50–53   Tower 3 decoder
ch 54–64   Tower 3 strobe
```

Total: 64 channels (`NUM_CHANNELS = 64` unchanged).
