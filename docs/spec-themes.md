# Spec: Tower Themes (idle visuals)

## Context

Earlier the per-tower idle look was a single FastLED gradient palette ([green/blue/natural fire](spec-per-tower-control.md)) flashed on an 800 ms / 3200 ms cycle, selected by a `palette` config field. This spec replaces that with a **theme** system: the same three fire gradients plus five procedural themes, a per-tower **speed** control, and a single renderer (`themeRender()`) shared in spirit with the dev simulator.

This is a rename + extension: `palette` → `theme` across firmware, JSON, storage, web UI, and tests. The cached `CRGBPalette256` per tower is dropped — the renderer computes RGBW per frame from the theme name.

---

## Theme catalogue

`themeRender(const String& name, uint8_t index, uint32_t nowMs, uint8_t brightness, uint16_t speedPct)` → `TowerState` (RGB + `white` + strobe-control fields). `index` is the tower 0–3.

| Name | Type | Behaviour |
|---|---|---|
| `green` | gradient | Green-fire palette, 800 ms ON / 3200 ms OFF flash cycle |
| `blue` | gradient | Blue-fire palette, same flash cycle |
| `fire` | gradient | Natural-fire palette, same flash cycle |
| `simon` | procedural | Global rotating R/B/Y/G across the 4 towers, 1 s/beat; tower `i` at beat `b` shows `SIMON[(i − b + 4) % 4]` |
| `rainbow` | procedural | Continuous hue rotation (~8 s/cycle), 90° (0.25) hue offset per tower |
| `warm_white` | procedural | Dim warm white on the uplight white channel (+ touch of red on RGB for warmth) |
| `bright_white` | procedural | Full white on the uplight white channel; RGB off |
| `candle` | procedural | Flickering warm white on the uplight white channel (summed-sine flicker per tower) |

Unknown names fall back to `green`.

### Colour vs. white

Colour themes drive RGB (`s.r/g/b`) and leave `s.white = 0`. White themes drive `s.white` (the uplight's dedicated white channel) and keep RGB low/off. This matters because white is a separate DMX channel from the fire valve — see [spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md). The fire gradients still keep `white = 0`; their white "tips" come from the gradient's RGB, not the W channel.

### Speed

`speedPct` (10–400, 100 = normal) scales effective time: `t = nowMs * speedPct / 100`. All time-based behaviour (flash cycle, Simon beat, rainbow hue, candle flicker) responds. `brightness` scales output magnitude as before.

### Strobe control

Every render sets `s.masterDim = 255` and `s.rgbStrobe = 1`. The LaluceNatz LL960S in 11-channel mode treats strobe CH2 = 0 as "RGB section off"; 1–7 is steady/open. Without `rgbStrobe = 1` the uplight shows no colour regardless of its RGB channels.

---

## Files

- **`themes.h` / `themes.cpp`** — replaces `palettes.h/.cpp`. Holds the three `DEFINE_GRADIENT_PALETTE` tables, `themeRender()`, and helpers (`palForName`, `hsvToRgb`, `flicker01`, `scaleTime`). `palFromName()` is gone.
- **`towers.h`** — `TowerConfig.palName` → `themeName`; `CRGBPalette256 pal` removed; `uint16_t speed` added.
- **`towers.cpp`** — defaults `themeName="green"`, `speed=100`.
- **`Test_Button_DMX.ino`** — IDLE/COOLDOWN arm calls `themeRender(themeName, i, millis(), bright, speed)`.
- **`storage.cpp`** — theme NVS key `t<N>h` (renamed from `t<N>p` — deliberate one-time wipe, old saved palette is not migrated and resets to `green`); speed key `t<N>s`.
- **`tools/web-preview/simulator.html`** — `renderTheme()` is the JS mirror; keep in lock-step.

---

## Web UI (`web.cpp` / `tools/web-preview/index.html`)

- `themeSelect("theme", …)` helper emits the 8-option `<select name='theme'>` (replaces `paletteSelect`).
- Per-tower and Apply-to-All forms gain a **Speed (%)** slider (`name='speed'`, 10–400, step 10, default 100).
- `handleSet()` parses `theme` and `speed` (clamped to 10–400, else 100) for `target=all` and per-tower.
- `handleApiState()` emits `"theme"` and `"speed"` per tower (was `"palette"`).

---

## Persistence

`themeName` (`t<N>h`) and `speed` (`t<N>s`) persist to NVS, saved on every config change like brightness/flameLevel. The key rename from `t<N>p` is intentional and un-migrated: after first boot post-upload, each tower's theme resets to `green` once, then persists normally.

---

## Non-goals

- **Per-tower phase/offset config for simon/rainbow.** Offsets are derived from tower index; not user-tunable.
- **Custom palettes / colour picker.** The eight themes are fixed; adding one is a branch in `themeRender()` + an option in `themeSelect()` + a mirror in `renderTheme()`.
- **Theme behaviour during `FIRE_ACTIVE`.** Themes render in all states; the fire valve and end-cue white are overlaid by the main loop (see [spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md)).
