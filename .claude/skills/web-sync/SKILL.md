---
name: web-sync
description: Port web UI changes from tools/web-preview/index.html (source of truth) into Test_Button_DMX/web.cpp's buildPage() F() strings. Use this skill whenever the user says "sync web", "web-sync", "update web.cpp from the preview", "port the HTML to the firmware", or anything that involves applying iterated web-preview changes into the firmware web handler. Do NOT use this skill to push changes the other direction — index.html is always the source of truth.
---

# Web UI Sync

The DMX Fire web interface lives in two places:

1. **Source of truth** — [tools/web-preview/index.html](../../../tools/web-preview/index.html). A nicely-formatted, fully-static HTML file that the local preview server (`scripts/web-debug.sh`, port 8123) serves to a browser for UI iteration.
2. **Firmware** — `buildPage()` in [Test_Button_DMX/web.cpp](../../../Test_Button_DMX/web.cpp). Builds the same HTML at runtime as concatenated `F()` strings, interleaved with C++ that injects current config values (mode `selected`, slider values, per-tower loop, etc.).

This skill ports static changes (CSS, JS, HTML structure, copy) from `index.html` into the `F()` strings in `web.cpp`, while preserving every dynamic C++ insertion exactly as-is.

## Files this skill touches vs. ignores

**Sources:**
- `tools/web-preview/index.html` — canonical firmware web UI. **Port this to firmware.**
- `tools/web-preview/server.py` — mock server JSON shape. **Mirror field renames into the firmware's `handleApiState()`.**

**NEVER port to firmware:**
- `tools/web-preview/simulator.html` — dev-only theme animation preview, served at `/simulator`. Lives only on the developer's laptop.
- Anything in `index.html` between `<!-- DEV-ONLY-START -->` and `<!-- DEV-ONLY-END -->` markers (e.g. the "Theme simulator →" link in the header). Strip these blocks completely when porting to `web.cpp`.

## Workflow

### 1. Read both files

- Read `tools/web-preview/index.html` (the new design).
- Read `Test_Button_DMX/web.cpp` — focus on `buildPage()` and the three helpers it calls: `themeSelect()` (renamed from `paletteSelect()`), `rangeSlider()`, `connectedCheck()`.

### 2. Identify what changed

Compare the static parts of the HTML. Things to watch for:

- **CSS** (the `<style>` block) — most common edit.
- **Static copy / structure** — section headings, button labels, fieldset legends, placeholder text, the wrapper markup around dynamic fields.
- **JavaScript** (the `<script>` block at the bottom) — event handlers, Test Fire bindings, mgRow visibility, morse handlers.
- **Helper-emitted markup** — if the HTML around sliders, theme selects, or checkboxes changed, the change belongs in `rangeSlider()` / `themeSelect()` / `connectedCheck()`, not the call site.

### 3. Identify what must NOT be ported as plain HTML

These are dynamic in `web.cpp` and must stay as C++ code, not be flattened into static `F()` strings:

- `<option ... selected>` on mode/endCue/theme — driven by `if (buttonConfig.mode == 0) s += F(" selected");`
- Slider `value=` attributes — emitted by `rangeSlider(..., buttonConfig.fireDurationMs, ...)` etc.
- Slider `<span class='val'>N</span>` text — also from `rangeSlider`.
- Checkbox `checked` — from `connectedCheck(..., towerConfigs[i].connected)`.
- `#mgRow style='display:none'` — driven by `if (buttonConfig.mode != 2)`.
- The four `Tower 0..3` fieldsets — emitted by a `for (uint8_t i = 0; i < NUM_TOWERS; i++)` loop.
- All `value='<int>'` on `<input type='hidden' name='target'>` for per-tower forms.

If the user changed a default value in `index.html` (e.g. `value='3000'` to `value='5000'`), that is a firmware default change — update `storage.cpp` defaults, not `web.cpp`. Confirm with the user before doing this.

### 3a. Pending firmware rename: `palette` → `theme`

The preview has been renamed `palette` → `theme` (form field name, JSON field, HTML labels, option list). Mirror this across the firmware in one coordinated change:

**Identifiers to rename in `Test_Button_DMX/`:**
- `TowerConfig.palName` → `TowerConfig.themeName` (in `towers.h`).
- `TowerConfig.pal` (the cached `CRGBPalette256`) → **remove**. The new theme renderer computes RGBW per-frame from `themeName` and doesn't need a cached palette.
- `paletteSelect()` helper in `web.cpp` → `themeSelect()`.
- `palFromName()` in `palettes.cpp` → **delete**; replaced by the new theme renderer.
- All `server.arg("palette")` calls in `web.cpp` → `server.arg("theme")`.
- `"palette":"..."` in `handleApiState()` → `"theme":"..."`.

**New per-tower field: `speed` (percentage, 100 = normal).** Scales time-based theme behaviour. Add to:
- `TowerConfig` in `towers.h`: `uint16_t speed;` (range 10..400; default 100).
- `storage.cpp`: new NVS key `"t%ds"`, default `100` on load, persisted on save.
- `web.cpp` `handleSet()`: parse `server.arg("speed").toInt()` for `target=all` and per-tower.
- `web.cpp` `handleApiState()`: emit `"speed":` per tower.
- `themeRender()` in `themes.cpp`: take `uint16_t speedPct` and scale effective time by `speedPct / 100` so e.g. the fire flash cycle, Simon beat, rainbow hue, and candle flicker all respond. Match the JS in `simulator.html` (the `simNowMs` accumulator there is scaled by the same percentage).

**NVS storage migration** (`storage.cpp`): rename the per-tower key from `"t%dp"` to `"t%dh"`. This is a deliberate one-time wipe — the existing saved theme on the device will reset to the default `"green"` on first boot after upload. No migration code needed.

**Files renamed:** `palettes.h` / `palettes.cpp` → `themes.h` / `themes.cpp`. Update `#include` lines across the sketch.

**New theme renderer** (`themes.cpp`):
- Add a function with this signature (mirror `tools/web-preview/simulator.html`'s `renderTheme()`):
  ```cpp
  // Returns the RGBW values for tower `index` at time `nowMs` for the given theme.
  // For flash-pattern themes returns black during the OFF phase.
  // `brightness` is the per-tower brightness from TowerConfig.bright.
  TowerState themeRender(const String& name, uint8_t index, uint32_t nowMs, uint8_t brightness);
  ```
- Existing gradient palettes (`firepal`, `electricGreenFirePal`, `electricBlueFirePal`) stay; they become one branch of the renderer.
- Add procedural branches: `simon`, `rainbow`, `warm_white`, `bright_white`, `candle`. Match the JS in `simulator.html` for semantics (Simon: tower `i` at beat `b` shows `SIMON[(i - b + 4) % 4]`; Rainbow: per-tower hue offset; Candle: warm-white + Perlin-ish flicker).

**Main loop change** (`Test_Button_DMX.ino`): replace the IDLE/COOLDOWN `default:` arm that hardcoded the 800 ms / 3200 ms flash + `ColorFromPalette(towerConfigs[i].pal, ...)` with a single call to `themeRender(towerConfigs[i].themeName, i, millis(), towerConfigs[i].bright)`. Fire-flash themes own their own 800/3200 cycle inside the renderer; non-flash themes (Simon, Rainbow, whites, Candle) render continuously.

**Tests to update** under `tests/`:
- `tests/api.py`: rename `palette=` kwarg/keys to `theme=` in `set_all_towers()` and `set_tower()`. Add `speed: int = 100` kwarg to both.
- All `test_*.py` files that assert on the `palette` field or POST `palette=` — rename to `theme`. Add the new valid theme names to any validation lists (`{"green","blue","fire","simon","rainbow","warm_white","bright_white","candle"}`).
- `conftest.py`: any baseline state with `palette=` → `theme=`. Add `speed=100` to defaults.
- `tests/visual/scripts/visual_test.py`: same rename.

### 3b. Check whether `/api/state` needs new fields

The browser-side JS may depend on JSON fields the firmware doesn't yet emit. Inspect the served `index.html` for any `/api/state` references and confirm every field consumed by the JS is also emitted by `handleApiState()` in `web.cpp`. Examples:

- `boot_id` — a per-boot fingerprint, used by the Test Fire arm cover so it auto-closes after a device reboot. Firmware needs to generate this once in `setup()` (e.g. with `esp_random()`) and emit it in `handleApiState()`.

If a field is missing, add it to `handleApiState()` and any necessary supporting code (boot-time setup, header declarations) as part of the same sync. The mock server in `tools/web-preview/server.py` is the reference shape.

### 4. Apply the changes to web.cpp

Use `Edit` to update the relevant `F("...")` chunks. Conventions to follow when writing back:

- Use single quotes for HTML attributes inside `F("...")` strings (so the outer C++ string can use double quotes without escaping).
- Keep multi-line C++ literal concatenation; don't put the entire page on one line.
- Preserve the existing minified style of the inline `<script>` (no indentation, single-line statements separated by `;` and `"` continuations) — the firmware ships kilobytes of HTML to a tiny RAM-constrained device, and that style is intentional. If `index.html` formats the JS prettily for editing, **minify it** before pasting into `web.cpp`.
- Keep `s.reserve(N)` in sync if the page grew substantially.

### 5. Compile-check

Run a compile to verify nothing broke:

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX
```

If it fails, fix the error and re-compile.

### 6. Report

Summarise to the user: which sections changed, which `F()` strings were edited in `web.cpp`, and any helper functions that needed updating. Mention compile result. Do NOT flash the device — that's a separate user-initiated step (`/upload`).

## What NOT to do

- **Never edit `index.html` to match `web.cpp`.** The sync direction is index.html → web.cpp, always.
- **Never strip dynamic C++ logic** out of `buildPage()`. If a slider's `value=` is hardcoded in `index.html`, that is a preview placeholder; the firmware must keep the dynamic insertion.
- **Never re-arrange fieldsets** in a way that changes the per-tower loop output unless the user explicitly asked. The loop emits Tower 0..3 in order; index.html mirrors that.
- **Never flash the device** as part of sync. Flashing is a separate operator action.
