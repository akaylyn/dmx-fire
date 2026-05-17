# Spec: Fire Lockout, Button Modes, Confluence Fixture & Config Improvements

## Context

The rig has two distinct fixture groups:

- **Simon towers (×4)**: RGBW LED decoder + 11ch strobe per tower. **CH4 of the decoder (the W channel) triggers a flame effect** — each tower has fire capability. CH1–3 are RGB visuals.
- **Confluence**: A single art-piece tower with a **real propane solenoid** on DMX CH4 (white). CH1–3 are wired but ignored by the fixture — only CH4 triggers fire. Sits first in the DMX chain.

The lockout is a **safety mechanism**: prevents solenoids from being held open continuously. The button triggers fire on **all connected towers** (Confluence CH4 + Simon tower CH4s) along with light patterns.

Priority order: **fire lockout + button modes → confluence fixture → EEPROM → auto-save UI → patterns**.

---

## Phase 1 — DMX remap: Confluence fixture first (prerequisite for everything)

Confluence occupies 4 DMX channels (to match the decoder format), but only CH4 matters — it opens the propane solenoid. CH1–3 are written but ignored by the fixture.

### New universe layout

```
Confluence:  ch  1– 4  (only ch 4 = solenoid; ch 1–3 ignored)
Tower 0:     ch  5–19  (decoder 5–8, strobe 9–19)
Tower 1:     ch 20–34
Tower 2:     ch 35–49
Tower 3:     ch 50–64
```

Total: 4 + 60 = 64 channels. `NUM_CHANNELS = 64` already covers this.

### New `confluence.h` / `confluence.cpp`

```cpp
struct ConfluenceConfig {
  bool    connected;   // physically present
  uint8_t fireLevel;   // 0=off, 255=full open (written to ch 4)
};
extern ConfluenceConfig confluenceConfig;

void confluenceSetup();
void confluenceWrite(uint8_t fireLevel);  // writes 0,0,0,fireLevel to ch 1–4
```

- `confluenceSetup()`: defaults to connected=true, fireLevel=255
- `confluenceWrite(level)`: writes `dmxDevice.writeByte(0, 1..3)` and `dmxDevice.writeByte(level, 4)`

### Simon tower flame support

Each `TowerConfig` gains a `flameLevel` field (written to CH4 / the W channel of the decoder):

```cpp
struct TowerConfig {
  // existing fields ...
  uint8_t flameLevel;  // 0=off, 255=full open; written to decoder ch 4 (W)
};
```

`towerWrite()` already writes all 4 decoder channels; `flameLevel` populates the W byte. The web UI gets a per-tower flame level slider (disabled if `connected=false`).

> **Universe mapping — Simon tower CH4:**
> - Tower 0: universe ch 8
> - Tower 1: universe ch 23
> - Tower 2: universe ch 38
> - Tower 3: universe ch 53

**`towers.cpp`**: shift all tower base addresses by +4:
```cpp
const uint16_t base = 4 + index * CHANNELS_PER_TOWER;
```

**`web.cpp`**: add a Confluence section (connected toggle + fire level slider — no palette, since there are no lights). Shown separately from the tower sections.

**`Test_Button_DMX.ino`**: call `confluenceSetup()` in `setup()`.

---

## Phase 2 — Fire lockout + button modes (highest priority)

Confluence fire is controlled via button press, with a state machine enforcing safety lockout.

### Button state machine

```
IDLE ──(press)──► FIRE_ACTIVE ──(timer expires OR release)──► END_CUE ──(cue done)──► COOLDOWN ──(timer)──► IDLE
                                                                                            └──(press during cooldown) ignored
```

States in new `button_fsm.h`:
- `IDLE` — waiting; towers show idle palettes; all CH4s = 0
- `FIRE_ACTIVE` — **Confluence CH4 = 255 AND each connected Simon tower CH4 = 255**; towers show fire palette; countdown ticking
- `END_CUE` — all CH4s → 0; towers play end-of-fire cue pattern (~1 s)
- `COOLDOWN` — locked out; idle running; button presses ignored until timer expires

### Button modes

Two modes selectable in the web UI:

| | `FIREBALL` | `PARTY` |
|--|-----------|---------|
| **Single press** | All fixtures fire for `fireDurationMs`, then lockout | Fire + randomised colour sequence on Simon towers |
| **Hold** | Solenoid open while held, up to `fireDurationMs` cap, then lockout | Same + continuous tower light show |
| **Double press** | 2× duration burst | TBD |

### `ButtonConfig` struct

```cpp
struct ButtonConfig {
  uint8_t  mode;           // 0=FIREBALL, 1=PARTY
  uint16_t fireDurationMs; // max solenoid open time (default 3000 ms)
  uint16_t cooldownMs;     // lockout before next trigger (default 10000 ms)
  uint8_t  endCuePattern;  // 0=white flash fade, 1=colour cascade
};
extern ButtonConfig buttonConfig;
```

### End-of-fire cue

Short (~1 s) tower-only pattern at transition to COOLDOWN — signals to the user that fire time is up. Minimum: fast white strobe across all towers, fade to black, then idle. Pattern is selectable in web UI.

### Double/single press detection

300 ms window after `wasReleased` to catch a second press. If second press → double-press action. If not → single-press action. Uses a `lastReleaseMs` timestamp.

### Randomness (PARTY mode)

On each trigger, randomly assign each Simon tower its own palette, and randomly vary which towers light up (1–4 active). Confluence always fires regardless.

---

## Phase 3 — EEPROM persistence

Use ESP32 **`Preferences`** library (included in core). No extra library needed.

**New `storage.h` / `storage.cpp`**:
```cpp
void storageLoad();  // call in setup() before webSetup()
void storageSave();  // call after any config change
```

What to persist:
- `towerConfigs[4]`: palName, bright, connected, flameLevel
- `confluenceConfig`: connected, fireLevel
- `buttonConfig`: mode, fireDurationMs, cooldownMs, endCuePattern

`storageSave()` called by `handleSet()` after any web config update, and after button config changes.

---

## Phase 4 — Auto-save web UI

Replace form submit + page reload with `fetch()` POSTs fired on `input` / `change` events. Page never reloads.

```javascript
document.querySelectorAll('form').forEach(function(form) {
  form.querySelectorAll('select, input[type=range], input[type=checkbox]').forEach(function(el) {
    el.addEventListener('change', function() {
      fetch('/set', { method: 'POST', body: new FormData(form) });
    });
  });
  form.addEventListener('submit', function(e) {
    e.preventDefault();
    fetch('/set', { method: 'POST', body: new FormData(form) });
  });
});
```

Server-side: `/set` returns `200 OK` with empty body instead of `303` redirect.

---

## Phase 5 — Light patterns

- **Flame palettes for Confluence-only events** (no Simon towers) — pre-built colour sequences mimicking fire for any LED-capable fixture later wired to ch 1–3
- **Random tower sequences** — PARTY mode assigns palettes + active towers randomly per button event
- **Default fixture animations** — some strobe fixtures have built-in DMX chase modes; can be exposed as a palette-equivalent option later

---

## Open questions / deferred

- **"Do we know if reds are configured correctly?"** — need to verify RGBW decoder CH1=R wiring matches physical fixture output
- **Double press behaviour** — TBD for both modes
- **Confluence web section** — same page as towers or separate "Fixtures" tab?
- **Simon towers sound** — explicitly deferred; hard-wired connection required
- **Max solenoid duty cycle** — is there a hardware-enforced max open time beyond the software lockout? (worth knowing before live use)

---

## Files modified / created

| File | Change |
|------|--------|
| `confluence.h` / `confluence.cpp` | New — solenoid config + DMX write (ch 1–4, only ch 4 active) |
| `towers.h` | Add `flameLevel` to `TowerConfig` |
| `towers.cpp` | Shift base address +4 for all towers; init `flameLevel = 0` |
| `button_fsm.h` / `button_fsm.cpp` | New — state machine + ButtonConfig |
| `storage.h` / `storage.cpp` | New — Preferences load/save |
| `web.cpp` | Confluence section, button config section, flame sliders, auto-save JS, 200 response |
| `Test_Button_DMX.ino` | Wire up FSM, confluenceSetup/Write, storageLoad |

---

## Verification

1. Compile clean
2. Confluence CH4 goes to 255 on button press, returns to 0 on release / timer expiry
3. Simon tower CH4s also go to their configured flameLevel on button press
4. DMX analyser confirms Tower 0 now starts at ch 5
5. Single press → fire for configured duration → end cue plays → cooldown (button ignored)
6. Press during cooldown → no effect
7. Power cycle → all config restored from EEPROM
8. Slider change in web UI → applies without page reload
