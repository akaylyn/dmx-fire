# Binary solenoid channels: fire is 0 or 255, never in between

## Context

Every propane valve on this rig is driven by a byte that is currently
**operator-settable to any value 0–255**:

- `TowerConfig.flameLevel` (per tower, NVS `t<N>f`, slider in the web UI)
- `ConfluenceConfig.fireLevel` (NVS `cffl`, slider in the web UI)

That byte reaches DMX CH 1 / 8 / 23 / 38 / 53 verbatim — there is no scaling
anywhere, confirmed end to end. So the slider does not make a smaller flame; it
only decides whether the decoder's turn-on threshold is cleared. Below it the
valve stays shut or the coil chatters. Flame size is gas pressure and orifice.

The codebase already knows this and has been papering over it: `towers.h:30-35`
documents "NOT a proportional flame control", the live web readout warns at
`< 128`, `docs/spec-live-fixture-state.md:58` prints a correction, and
`notes.md:192` records a field session lost to it. `scripts/towers.sh` exists in
part to flag `flameLevel=0`. A control that every layer has to warn about should
not exist.

**Outcome:** a valve channel can only ever carry 0 or 255. Partial values become
unrepresentable in the firmware's types *and* rejected at the DMX write, and no
API or UI surface offers a number to set. Per-fixture propane isolation survives
as a boolean (`fireEnabled`), so a leaking tower can still be shut off without
going dark.

## Scope of the fact-find (already done)

- `flameLevel` / `fireLevel` appear in **9 firmware files, 12 pytest files,
  4 tools, 16 docs**. Full inventory gathered; every site is named below.
- Valve-ness is currently declared nowhere in the firmware — it is an offset
  convention (`base + 4`, `base = 4 + i*15`) plus the literal `1`, restated by
  hand in six places. The only machine-readable list in the repo is
  `tools/dmx-tester/index.html:153` — `VALVE_CHANNELS = [1, 8, 23, 38, 53]`.

---

## 1. `dmx.h` / `dmx.cpp` — declare valve channels as data, enforce binary

The load-bearing piece. Add to `Test_Button_DMX/dmx.h`:

```cpp
// Channels wired to a propane solenoid. A solenoid is a binary device: the
// decoder has a turn-on threshold, and a byte near it either fails to energise
// the coil or chatters it. Flame size is gas pressure and orifice, not DMX.
// Mirrors tools/dmx-tester/index.html:153 and docs/hardware.md.
static const uint8_t  NUM_VALVE_CHANNELS = 5;
extern const uint16_t VALVE_CHANNELS[NUM_VALVE_CHANNELS];   // {1, 8, 23, 38, 53}

static const uint8_t VALVE_CLOSED = 0;
static const uint8_t VALVE_OPEN   = 255;

bool dmxIsValveChannel(uint16_t ch);
void dmxValveWrite(uint16_t ch, bool open);   // the ONLY way to open a valve
```

In `dmx.cpp`, `dmxShadowWrite()` gains the guard — this is what makes the rule
structural rather than conventional:

```cpp
void dmxShadowWrite(uint8_t value, uint16_t ch) {
  if (ch < 1 || ch > DMX_SHADOW_SIZE) return;
  // A valve channel takes 0 or 255 and nothing else. Refuse rather than clamp:
  // a caller that computed 128 for a solenoid has a bug, and silently rounding
  // it to "open" is the wrong direction to guess on propane.
  if (dmxIsValveChannel(ch) && value != VALVE_CLOSED && value != VALVE_OPEN) {
    LOG_E("[DMX] refused %u on valve CH%u — valves are 0 or 255 only", value, ch);
    return;
  }
  dmxLastFrame[ch - 1] = value;
}
```

`dmxValveWrite(ch, open)` is a thin `dmxShadowWrite(open ? VALVE_OPEN : VALVE_CLOSED, ch)`.
`dmx.cpp` will need `#include "log.h"`.

## 2. Types — make a partial value unrepresentable

| File | Change |
|---|---|
| `towers.h` | `TowerState`: `uint8_t fire` → `bool fireOpen` |
| `towers.h` | `TowerConfig`: drop `uint8_t flameLevel`, add `bool fireEnabled` |
| `towers.cpp` | `towerSetup()`: `fireEnabled = true` (was `flameLevel = 255`) |
| `towers.cpp:55` | `dmxShadowWrite(state.fire, base+4)` → `dmxValveWrite(base + 4, state.fireOpen)` |
| `confluence.h/.cpp` | `ConfluenceConfig`: drop `fireLevel`, add `bool fireEnabled`; `confluenceWrite(uint8_t level)` → `confluenceWrite(bool open)`, using `dmxValveWrite(1, open)` |
| `morse.h/.cpp` | `uint8_t morseTick()` → `bool morseTick()`; returns `g_seq[idx] == '1'` (drops the `confluenceConfig.fireLevel` reference at `morse.cpp:84`) |

## 3. `Test_Button_DMX.ino` — the frame block

Replace the two `state.fire` assignments and the `cfLevel` chain:

```cpp
if (firing) {
  state.fireOpen = mgOn && towerConfigs[i].fireEnabled;
  applyFireLook(state);
}
...
if (purge) {
  state.fireOpen = towerConfigs[i].fireEnabled;
  applyFireLook(state);
}
```

Confluence:

```cpp
if (confluenceConfig.connected) {
  bool cfOpen = false;
  if (purge)              cfOpen = confluenceConfig.fireEnabled;
  else if (morseActive()) cfOpen = morseTick() && confluenceConfig.fireEnabled;
  else if (firing)        cfOpen = mgOn && confluenceConfig.fireEnabled;
  confluenceWrite(cfOpen);
} else {
  confluenceWrite(false);   // see below
}
```

**Related safety fix, in scope.** Today `if (!towerConfigs[i].connected) continue;`
(`.ino:209`) and `if (confluenceConfig.connected)` (`.ino:252`) mean a
disconnected fixture's valve channel is **not written at all** — it latches the
last byte. Un-tick "Connected" on a tower mid-burn and CH8 stays at 255 with
nothing left to close it. Now that the valve channels are addressable data, fix
it directly:

```cpp
for (uint8_t i = 0; i < NUM_TOWERS; i++) {
  if (!towerConfigs[i].connected) {
    dmxValveWrite(4 + i * 15 + 4, false);  // never leave a valve latched open
    continue;
  }
  ...
```

`ota.cpp:45` and `tests.cpp:142,150` change `confluenceWrite(0)` →
`confluenceWrite(false)`; their zeroed `TowerState off = {}` already yields
`fireOpen == false`.

## 4. Persistence (`storage.cpp`)

- Tower: new key `t<N>v` (bool, default `true`). `char key[4]` has room.
- Confluence: new key `cffe` (bool, default `true`).
- **Do not** reuse `t<N>f` / `cffl` — they hold `UChar` entries and `getBool()`
  on a type-mismatched key returns the default, which is fragile to rely on.
  Instead call `prefs.remove()` on both old keys in `storageSave()` so a rig
  that has run the old firmware sheds them on the first config write. No
  `--erase` required.

## 5. Web UI — `tools/web-preview/index.html` first, then port

`index.html` is the source of truth; use the **`/web-sync` skill** to port into
`web.cpp`'s `buildPage()` F() strings. Do not edit `web.cpp` HTML by hand.

- **Confluence tab:** delete the `fireLevel` range input (`index.html:671`,
  `web.cpp:413`). Add a "Fire enabled" checkbox using the existing
  `connectedCheck()` helper pattern (`web.cpp:78-95` has `rangeSlider`; the
  checkbox helper is alongside).
- **Towers tab:** delete all five `flameLevel` sliders — Apply-to-All
  (`index.html:733`, `web.cpp:451`) and the four per-tower ones
  (`index.html:772/810/848/886`, `web.cpp:487`). Add a per-tower "Fire enabled"
  checkbox next to Connected. **Not** on the Apply-to-All form, matching how
  `connected` is already excluded there.
- **Live readout** (`index.html:1367-1410`, `web.cpp:766-795`): delete the
  `flameLevel === 0` / `< 128` and `fireLevel === 0` / `< 128` warning branches
  entirely. Replace with a single `fireEnabled === false` warning, and add a new
  one that flags any valve byte in `dmx.ch` that is neither 0 nor 255 — which
  after this change should be impossible, so it reads as a firmware fault.

## 6. HTTP API (`web.cpp`)

- `handleSet()`: drop `web.cpp:890` (`fireLevel`), `:924/929` (`all` flameLevel),
  `:944` (per-tower flameLevel). Add `fireEnabled = server.hasArg("fireEnabled")`
  for `target=confluence` and `target=0..3`. Legacy `flameLevel=` params become
  inert; the request still returns 200.
- `/api/state`: drop `web.cpp:1163-1164` and `:1258-1259`; emit `fireEnabled`
  in `confluence{}` and each `towers[]` entry.

Resulting shape:

```json
"confluence": { "connected": true, "fireEnabled": true },
"towers": [ { "connected": true, "fireEnabled": true,
              "theme": "green", "brightness": 128, "speed": 100 } ]
```

## 7. Tests

### New — `tests/test_valve_binary.py`

The direct proof of the new contract. Reuses `ch()` and the channel constants
from `tests/test_dmx_output.py:17-30`; promote those to a shared
`tests/valves.py` (`CONFLUENCE_FIRE_CH = 1`, `TOWER_FIRE_CH = [8, 23, 38, 53]`,
`VALVE_CHANNELS`) so `test_dmx_output.py`, `test_audio.py:22`,
`test_dmx_quiet.py:33` and the new file stop hand-copying it.

| Test | Asserts |
|---|---|
| `test_valves_binary_during_fire` | hold FIRE_ACTIVE, poll `/api/state` ~20×; every sample of all 5 valve channels ∈ `{0, 255}` |
| `test_valves_binary_during_purge` | same under `/api/purge/start` |
| `test_valves_binary_during_machine_gun` | mode 2, sample across bursts; observed set is **exactly** `{0, 255}` — proves pulsing is still binary, not a ramp |
| `test_valves_binary_during_morse` | CH1 across a morse message ⊆ `{0, 255}` |
| `test_legacy_flame_level_param_is_inert` | `POST /set target=0 flameLevel=200` → fire → CH8 == 255, not 200 |
| `test_state_has_no_level_fields` | `"flameLevel"` absent from every tower dict, `"fireLevel"` absent from `confluence` |
| `test_fire_enabled_false_keeps_valve_shut` | tower 0 `fireEnabled=False` → CH8 == 0 during fire, while CH9–12 still show the fire look and towers 1–3 read 255 |
| `test_confluence_fire_enabled_false` | CH1 == 0 while tower valves open |
| `test_fire_enabled_round_trips` | set/read-back via `/api/state`, distinct per tower |
| `test_disconnected_tower_valve_forced_closed` | fire, disconnect tower 1 mid-run, CH23 → 0 (the `.ino:209` latch fix) |

### Firmware boot diagnostics — `Test_Button_DMX/tests.cpp`

Two additions to `runDiagnostics()`, so the invariant is checked on hardware
without the Python harness:

- `testValveChannelMap()` — for each tower, assert the computed `4 + i*15 + 4`
  is in `VALVE_CHANNELS`, and `dmxIsValveChannel()` is true for exactly
  `{1,8,23,38,53}` and false for a sample of neighbours (7, 9, 2, 24).
- `testValveGuardRefusesPartial()` — call `dmxShadowWrite(128, 8)` and assert
  `dmxLastFrame[7]` is unchanged; then `dmxValveWrite(8, true)` → 255,
  `dmxValveWrite(8, false)` → 0. Leaves the buffer at 0. This is the unit proof
  of §1.

Update `testTowerConfig()` (`tests.cpp:36-41`) and `testConfluenceConfig()`
(`:54-61`) to print/check `fireEnabled` instead of the level fields.

### Existing tests to update

- `tests/api.py:217` `set_confluence(*, connected, fireEnabled: bool = True)`;
  `:261` `set_all_towers` loses `flameLevel`; `:282` `set_tower` gains
  `fireEnabled: bool = True`. **Default `True` matters** — `/set` reads these
  with `hasArg`, so a caller that omits the field would otherwise shut off
  propane, the same trap `connected` already has.
- `tests/conftest.py:57-61` baseline: `fireEnabled=True` everywhere.
- `tests/test_dmx_output.py` — assertions change from `== 180` / `== 200` to
  `== 255`; `test_machine_gun_pulses_tower_valves` expects `{0, 255}`;
  `:90` `flameLevel=0` becomes `fireEnabled=False`.
- `tests/test_storage.py:10-22`, `test_state.py:28/37/55/61`,
  `test_config_per_tower.py`, `test_config_all_towers.py`,
  `test_config_confluence.py`, `test_audio.py:217/572/596`,
  `tests/visual/scripts/visual_test.py` — drop the level kwargs/assertions.
- `tests/README.md:45-54` table wording.

## 8. Tools

- `scripts/towers.sh` — replace the `flame` column with `fire` (enabled/off);
  drop the `flameLevel=0` problem string (`:90-92`); add a flag for any valve
  byte in `ch[]` that is neither 0 nor 255.
- `tools/web-preview/server.py` — mock: `:90/94-97` defaults, `:184/190/201`
  `/set` handling, `:218/227` `compose_dmx()` → `255 if firing else 0`.
- `tools/web-preview/simulator.html:630` — drop the `flameLevel` append.
- `tools/dmx-tester/index.html` — already binary (`FULL`), but it drives real
  solenoids over the Enttec: add the same guard to `setCh()` (`:264`) using its
  existing `VALVE_CHANNELS` constant.

## 9. Docs

**New:** `docs/spec-solenoid-binary.md`, following the house format — Context,
valve channel registry + `dmxValveWrite()`, `fireEnabled` replacing
`flameLevel`/`fireLevel`, web UI changes, persistence (new keys + old-key
removal), and Non-goals (**no proportional flame control, ever; no purge level;
no per-shot intensity; MACHINE_GUN gates time, not amplitude**).

**Update** (each currently states a level drives the valve): `docs/hardware.md:58`,
`spec-tower-fixtures-fire-white.md`, `spec-fire-uplight.md:95-110`,
`spec-uplight-4ch-mode.md:38`, `spec-machine-gun.md:42,88`,
`spec-purge-accumulator.md:38-42,99`, `spec-morse-code.md:41`,
`spec-confluence-addressing.md:29,80,122,134`, `spec-live-fixture-state.md:48-58`
(its `< 128` warning table is deleted, not reworded), `spec-rapid-retrigger.md:123`,
`spec-api-button-and-tests.md:34-36`, `docs/manuals/dmx512-decoder.md:81,98`,
plus `CLAUDE.md:125,134` and `README.md:88`.

**Leave as history** with a one-line "superseded by spec-solenoid-binary.md"
note: `docs/spec-fire-lockout-confluence.md`,
`docs/impl-fire-lockout-confluence.md`.

---

## Verification

1. **Compile:** `arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX`
   — the type changes (`bool fireOpen`, `confluenceWrite(bool)`, `morseTick()`)
   surface every missed call site as a build error, which is the point.
2. **Simulator first** (per the standing workflow — no device flash unless asked):
   `tools/web-preview/server.py` + `index.html`, exercise the Confluence and
   Towers tabs, confirm no level slider remains and the live readout shows
   valves at 0/255 only.
3. **Boot diagnostics on hardware:** `runDiagnostics()` prints the new
   `testValveChannelMap` / `testValveGuardRefusesPartial` PASS lines over serial
   at 115200.
4. **API suite** (device powered, workstation on its AP):
   `scripts/test.sh --api`, then `pytest tests/test_valve_binary.py -v`.
5. **Wire check:** `scripts/towers.sh` during a Test Fire — the
   `valve channels CH1= CH8= CH23= CH38= CH53=` line must read only 0 or 255,
   and report no flagged problems.

## Housekeeping

Plan mode forced this file into `~/.claude/plans/`. Per the standing convention
(plans live in the repo), the first implementation step is to copy it to
`docs/plan-solenoid-binary.md` and commit it there.

## Explicit non-goals

- No change to uplight white, the fire-look colour, `STRIP_BRIGHTNESS_PCT`, or
  any RGB channel — those are lights and stay fully dimmable.
- No change to FSM timing, `fireDurationMs`, `cooldownMs`, `endCueMs`, or the
  audio limiter. This changes the *amplitude* rule only.
- No device flash as part of this work unless explicitly requested.
