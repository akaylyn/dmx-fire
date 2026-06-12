---
name: web-sync
description: Port web UI changes from tools/web-preview/index.html (source of truth) into Test_Button_DMX/web.cpp's buildPage() F() strings. Use this skill whenever the user says "sync web", "web-sync", "update web.cpp from the preview", "port the HTML to the firmware", or anything that involves applying iterated web-preview changes into the firmware web handler. Do NOT use this skill to push changes the other direction — index.html is always the source of truth.
---

# Web UI Sync

The DMX Fire web interface lives in two places:

1. **Source of truth** — [tools/web-preview/index.html](../../../tools/web-preview/index.html). A nicely-formatted, fully-static HTML file that the local preview server (`scripts/web-debug.sh`, port 8123) serves to a browser for UI iteration.
2. **Firmware** — `buildPage()` in [Test_Button_DMX/web.cpp](../../../Test_Button_DMX/web.cpp). Builds the same HTML at runtime as concatenated `F()` strings, interleaved with C++ that injects current config values (mode `selected`, slider values, per-tower loop, etc.).

This skill ports static changes (CSS, JS, HTML structure, copy) from `index.html` into the `F()` strings in `web.cpp`, while preserving every dynamic C++ insertion exactly as-is.

## Workflow

### 1. Read both files

- Read `tools/web-preview/index.html` (the new design).
- Read `Test_Button_DMX/web.cpp` — focus on `buildPage()` and the three helpers it calls: `paletteSelect()`, `rangeSlider()`, `connectedCheck()`.

### 2. Identify what changed

Compare the static parts of the HTML. Things to watch for:

- **CSS** (the `<style>` block) — most common edit.
- **Static copy / structure** — section headings, button labels, fieldset legends, placeholder text, the wrapper markup around dynamic fields.
- **JavaScript** (the `<script>` block at the bottom) — event handlers, Test Fire bindings, mgRow visibility, morse handlers.
- **Helper-emitted markup** — if the HTML around sliders, palette selects, or checkboxes changed, the change belongs in `rangeSlider()` / `paletteSelect()` / `connectedCheck()`, not the call site.

### 3. Identify what must NOT be ported as plain HTML

These are dynamic in `web.cpp` and must stay as C++ code, not be flattened into static `F()` strings:

- `<option ... selected>` on mode/endCue/palette — driven by `if (buttonConfig.mode == 0) s += F(" selected");`
- Slider `value=` attributes — emitted by `rangeSlider(..., buttonConfig.fireDurationMs, ...)` etc.
- Slider `<span class='val'>N</span>` text — also from `rangeSlider`.
- Checkbox `checked` — from `connectedCheck(..., towerConfigs[i].connected)`.
- `#mgRow style='display:none'` — driven by `if (buttonConfig.mode != 2)`.
- The four `Tower 0..3` fieldsets — emitted by a `for (uint8_t i = 0; i < NUM_TOWERS; i++)` loop.
- All `value='<int>'` on `<input type='hidden' name='target'>` for per-tower forms.

If the user changed a default value in `index.html` (e.g. `value='3000'` to `value='5000'`), that is a firmware default change — update `storage.cpp` defaults, not `web.cpp`. Confirm with the user before doing this.

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
