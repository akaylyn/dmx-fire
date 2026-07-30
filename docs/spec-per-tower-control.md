# Spec: Per-Tower Independent Control via Web UI

> **Historical design spec — code samples below are superseded.** This records the
> original migration from single-tower globals to per-tower config. Since then the
> palette fields became the theme system (`themeName`/`speed`, see
> [spec-themes.md](spec-themes.md)), `wDim` was split into separate fire and white
> channels ([spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md)),
> and the uplights moved to 4-channel mode, dropping `masterDim`/`rgbStrobe`/`wStrobe`
> from `TowerState` entirely ([spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md)).
> The snippets are kept as-written for the record; for current behaviour see
> `docs/hardware.md` and `CLAUDE.md`.

## Overview

Each of the 4 towers gets its own palette and brightness, configurable via the web UI. A "set all" control at the top allows ganging all towers at once. A "connected" toggle per tower tracks which fixtures are physically present.

---

## Data Model

Replace the single `idlePal`/`idleBright`/`currPal`/`currBright` globals with a per-tower config array.

### New `TowerConfig` struct (`towers.h`)

```cpp
struct TowerConfig {
  bool            connected;   // shown as connected in web UI
  String          palName;     // "green", "blue", "fire" — for web UI rendering
  CRGBPalette256  pal;         // active palette
  uint8_t         bright;      // brightness 0–255
};

extern TowerConfig towerConfigs[NUM_TOWERS];
extern bool        towerConfigUpdated;
```

Default (defined in `towers.cpp`): all towers connected, `electricGreenFirePal`, brightness 16.

### Globals removed

`idlePal`, `idleBright`, `idlePalName`, `currPal`, `currBright`, `idleUpdated` — all superseded by `towerConfigs[]`.

---

## Palette Helper (`palettes.h` / `palettes.cpp`)

Add:

```cpp
CRGBPalette256 palFromName(const String& name);
```

Maps `"green"` → `electricGreenFirePal`, `"blue"` → `electricBlueFirePal`, `"fire"` → `firepal`.

Replaces the repeated if-chains in web handler and main loop.

---

## Web UI

Switch `handleRoot()` from a static PROGMEM string to dynamic String generation — too many per-tower placeholders to template statically, and ESP32 has RAM to spare.

### Layout

```
╔══════════════════════════════╗
║  All Towers                  ║
║  Palette: [dropdown]         ║
║  Brightness: [slider]  [Set] ║
╠══════════════════════════════╣
║  Tower 0  [✓ Connected]      ║
║  Palette: [dropdown]         ║
║  Brightness: [slider] [Save] ║
╠══════════════════════════════╣
║  Tower 1  [ Connected]       ║
║  Palette: [dropdown]         ║
║  Brightness: [slider] [Save] ║
╠══════════════════════════════╣
║  Tower 2  [✓ Connected]      ║
║  ...                         ║
╠══════════════════════════════╣
║  Tower 3  [✓ Connected]      ║
║  ...                         ║
╚══════════════════════════════╝
```

Each section is its own `<form method="POST" action="/set">` with a hidden field:

```html
<input type="hidden" name="target" value="all">   <!-- or "0", "1", "2", "3" -->
```

### `handleSet()` logic

| `target` | Behaviour |
|----------|-----------|
| `"all"` | Apply `palette` + `brightness` to all 4 towers. Do not touch `connected`. |
| `"0"`–`"3"` | Apply `palette`, `brightness`, and `connected` (absent = false) to that tower. |

Set `towerConfigUpdated = true` in both cases.

Note: unchecked HTML checkboxes do not submit — absence of `connected` arg means `false`.

---

## Main Loop Changes (`Test_Button_DMX.ino`)

### Remove

- `if (idleUpdated && !keyButton.isPressed())` block
- All `currPal` / `currBright` references
- `currPal = electricBlueFirePal` on `wasPressed`
- `currPal = idlePal` on `wasReleased`

### Replacement DMX output block

```cpp
EVERY_N_MILLISECONDS(20) {
  static uint8_t currIndex = 0;
  bool held = keyButton.isPressed();

  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    CRGBPalette256& pal    = held ? electricBlueFirePal : towerConfigs[i].pal;
    uint8_t         bright = held ? 255                 : towerConfigs[i].bright;

    CRGB c = ColorFromPalette(pal, currIndex, bright, LINEARBLEND);

    TowerState state;
    state.r         = c.r;
    state.g         = c.g;
    state.b         = c.b;
    state.masterDim = 255;
    state.wDim      = bright;
    state.rgbStrobe = held ? 128 : 0;
    state.wStrobe   = held ? 128 : 0;

    towerWrite(i, state);
  }
  currIndex++;
  dmxDevice.update();  // flush once after all towers written
}
```

`towersWrite()` is no longer called from the main loop. `towerWrite()` per tower + one `dmxDevice.update()` replaces it.

---

## Files Modified

| File | Change |
|------|--------|
| `Test_Button_DMX/towers.h` | Add `TowerConfig` struct, extern array + updated flag |
| `Test_Button_DMX/towers.cpp` | Define `towerConfigs[4]` with defaults |
| `Test_Button_DMX/palettes.h` | Add `palFromName()` decl; remove old idle/curr globals |
| `Test_Button_DMX/palettes.cpp` | Implement `palFromName()`; remove old globals |
| `Test_Button_DMX/web.cpp` | Dynamic HTML generation; updated `handleSet()` |
| `Test_Button_DMX/Test_Button_DMX.ino` | Per-tower DMX loop; simplified button handling |

---

## Verification

1. `arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX` — must compile clean
2. Upload and connect to `dmx-fire` AP → open 192.168.4.1
3. Global "Set All" updates all 4 tower sections simultaneously
4. Changing Tower 0 palette/brightness does not affect Towers 1–3
5. Unchecking "Connected" on a tower persists after page reload
6. Holding button overrides all towers with blue fire + strobe; release restores per-tower config
7. Serial monitor shows `Startup.` and `AP: dmx-fire`
