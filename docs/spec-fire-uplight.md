# Spec: Fire-aligned uplights

## Context

During `FSM_FIRE_ACTIVE` the main loop set only `state.fire` and deliberately left
colour alone — the uplight stayed 100% theme-driven while the valve was open. The
default themes (`green` / `blue` / `fire`) are flash-pattern gradients that return a
**zeroed** `TowerState` for 3200 ms of every 4000 ms cycle, so during a 3 s burn the
uplights were dark for roughly 80% of it. The Empty Accumulator purge was worse: it
held every valve on the rig open with no visual change at all.

The rule now is simple and holds everywhere: **an open solenoid means a lit uplight.**
While any valve is open, that tower's uplight snaps to a configurable warm flame
colour and holds it steady for the whole event.

The accumulator LED strips are **not** part of this. They keep running their theme
underneath, so the tower still animates while the uplight reads as fire.

Related: [spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md) (channel map, the
4-channel trap), [spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md)
(fire/white channel separation), [spec-rapid-retrigger.md](spec-rapid-retrigger.md)
(the companion change to shot rate), [spec-purge-accumulator.md](spec-purge-accumulator.md).

---

## `TowerState` gains separate uplight RGB

`towerWrite()` writes the theme colour twice: capped to 75% for the accumulator
decoder, uncapped for the uplight. Both read the same `r`/`g`/`b`, so there was no
way to move one without the other. The uplight now has its own fields:

```cpp
struct TowerState {
  uint8_t r, g, b;      // theme colour — accumulator strips only (capped in towerWrite)
  uint8_t ur, ug, ub;   // uplight RGB (full) — theme colour, or the fire look while firing
  uint8_t white;        // uplight white channel (4-ch mode CH4) — independent of fire
  uint8_t fire;         // accumulator decoder CH4 — propane valve, FIRE_ACTIVE/purge only
};
```

### ⚠️ Still no `masterDim` / `rgbStrobe` / `wStrobe`

The warning in [spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md) is unchanged and
this split does not weaken it. In 4-channel mode the first two slots of the uplight
block are **Red and Green**; reintroducing an 11-channel-style dimmer/strobe pair
pins every uplight at red. `ur`/`ug`/`ub` are colour, nothing else.

`tests/test_dmx_output.py::test_uplight_is_four_channel_rgbw` remains the regression
guard and passes unchanged — the split does not alter what any theme emits.

### `themes.cpp` — themes stay unaware of the split

`themeRender()` has seven early returns plus a recursive unknown-theme fallback.
Rather than touch each one, the body moved to a `static themeRenderInner()` and the
public entry point became a wrapper:

```cpp
TowerState themeRender(const String& name, uint8_t index, uint32_t nowMs,
                       uint8_t brightness, uint16_t speedPct) {
  TowerState s = themeRenderInner(name, index, nowMs, brightness, speedPct);
  s.ur = s.r;
  s.ug = s.g;
  s.ub = s.b;
  return s;
}
```

No theme logic changed. Themes remain the single source of idle colour, and by
default the uplight shows exactly what the strips show.

---

## The fire look

A file-local helper in `Test_Button_DMX.ino`, applied from both fire paths:

```cpp
static inline void applyFireLook(TowerState& s) {
  s.ur    = buttonConfig.fireUpR;
  s.ug    = buttonConfig.fireUpG;
  s.ub    = buttonConfig.fireUpB;
  s.white = buttonConfig.fireUpW;
}
```

White is **assigned**, not `max()`'d against the theme. The fire look owns the
uplight while a valve is open, and the end-cue fade runs strictly afterwards — the
two never overlap.

### Per-state behaviour

| State | Uplight | Strips | Valve |
|---|---|---|---|
| `IDLE` / `COOLDOWN` | theme | theme | 0 |
| `FIRE_ACTIVE` | **fire look, held** | theme | `flameLevel` |
| `END_CUE` | theme + white flash fade | theme | 0 |
| Purge (any FSM state) | **fire look, held** | theme | `flameLevel` |

Precedence in the frame loop is unchanged: theme renders first, the FSM overlays on
top, and **purge wins over the FSM**.

### Machine gun: the uplight does not strobe

In `MACHINE_GUN` mode every valve pulses (see
[spec-rapid-retrigger.md](spec-rapid-retrigger.md)), but the uplight is held
**steady lit for the entire `FIRE_ACTIVE` window**. Only `state.fire` follows the
pulse gate:

```cpp
state.fire = mgOn ? towerConfigs[i].flameLevel : 0;
applyFireLook(state);   // NOT gated on mgOn
```

Strobing the uplight at the burst rate reads as a flicker fault on a bus that has a
documented history of exactly that, and is a photosensitivity hazard at short burst
lengths. "Solenoid open" is treated as the fire *event*, not the individual pulse.

---

## Configuration

**One global setting for the whole rig**, not per tower. It lives in `ButtonConfig`
because that struct already holds fire behaviour and already has web and NVS
plumbing:

```cpp
uint8_t fireUpR, fireUpG, fireUpB;  // uplight colour while a valve is open
uint8_t fireUpW;                    // uplight white level during fire
```

Defaults: **`255, 110, 0, 0`** — amber, white off. White defaults to 0 so the amber
stays saturated; raising it trades colour for total output.

### Web UI

New **Fire Uplight** fieldset in the Button Config tab, its own form with
`target=fireup`:

- `<input type='color' name='fireUpColor'>` — posts `#rrggbb`
- White slider, `fireUpW`, 0–255

`handleSet()` accepts either the colour-input shape or explicit `fireUpR/G/B/W` byte
fields, so tests can post either. A malformed `#rrggbb` is rejected outright rather
than partially applied, so a bad POST can never blank the fire look.

Source of truth is `tools/web-preview/index.html`, mirrored into `web.cpp`'s
`buildPage()` via `/web-sync`.

### `/api/state`

New top-level object alongside `button`:

```json
"fireUplight": { "r": 255, "g": 110, "b": 0, "w": 0 }
```

---

## Persistence

Four new global NVS keys in `storage.cpp`: `fireupr`, `fireupg`, `fireupb`,
`fireupw`. Global keys, so the per-tower `char key[4]` buffer is untouched. Defaults
are applied on load, so an existing device picks up amber without a `--erase`.

---

## Simulator

`tools/web-preview/simulator.html` mirrors the split: the uplight lamp paints
`ur/ug/ub` and takes the fire look while firing; the strip lamp stays on
`f.r/g/b × STRIP_PCT` and is never touched by it. New controls for fire colour and
fire white.

A pre-existing divergence was fixed at the same time: the gradient branch of
`renderTheme()` returned `w: brightness` while `themes.cpp` sets `s.white = 0` for
`green`/`blue`/`fire`. The simulator was painting white the fixtures never receive,
contradicting [spec-themes.md](spec-themes.md) and its own header comment. It now
returns `w: 0`.

---

## Tests

New in `tests/test_dmx_output.py`:

- **`test_uplight_shows_fire_look_while_firing`** — samples across more than one full
  4000 ms flash cycle and asserts every uplight reads exactly `(255, 110, 0, 0)` on
  every sample, including the theme's OFF phase. This is the direct regression guard
  for the dark-uplight bug.
- **`test_fire_look_does_not_touch_strips`** — uplight shows the fire look while strip
  red still reads the theme value × 75%.
- **`test_uplight_returns_to_theme_after_fire`** — back on theme in `COOLDOWN`.
- **`test_purge_lights_uplights_and_opens_all_valves`** — purge lights the uplights
  and opens all five valves.

New in `tests/test_config_button.py`: **`test_fire_uplight_round_trip`**.
`tests/conftest.py` pins the fire look to the amber default before each test.

---

## Non-goals

- **No per-tower fire colour.** One global setting; all four towers match while
  firing. Per-tower would mean four more NVS keys and four more pickers for a
  distinction the rig does not need.
- **No strip override during fire.** The accumulator strips keep running the theme.
  They are old and power-limited (`STRIP_BRIGHTNESS_PCT = 75`), and the visual
  intent is uplight-as-flame over an animated tower, not both going solid amber.
- **No uplight strobing in machine-gun mode.** See above.
- **No change to the end cue's white flash.** The fire look snaps off when the valve
  closes and the existing fade runs as before, now scaled to `endCueMs`.
- **No fire look for Morse.** Morse drives Confluence CH1 only and never opens a
  tower valve, so no tower uplight lights during it — consistent with the rule.
- **No `masterDim`/`rgbStrobe` revival.** See the warning above.
