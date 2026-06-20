# Spec: Per-tower fixtures, DMX addressing, fire/white separation

## Context

Each tower has **two DMX fixtures**, not one:

1. **Accumulator decoder** — a 4-channel decoder. CH1–3 drive the RGB LED strips wrapped around the accumulator tank; **CH4 is the propane fire valve only**. The strips are RGB-only, old, and power-limited (sending full R+G+B for white overdraws the supply), so their colour is capped.
2. **Uplight** — a LaluceNatz LL960S in 11-channel mode. Full theme colour on its RGB channels plus a **dedicated white channel** (CH11 of its block) that is safe to drive.

Two problems this spec fixes:

- **Fire forced white.** Previously the per-tower CH4 carried "white" (`wDim`), which was both the strip-decoder's W and the valve — firing lit everything white, and the boot diagnostic pulsed all valves. Fire and white are now separate channels.
- **All towers showed the same colour.** `towerWrite` wrote every tower to the same base address, so per-tower theme rendering (e.g. `simon`) wasn't visible. Each tower now has its own address block.

Goal: **fire without white, white without fire**, and per-tower independent colour.

---

## DMX address layout

Central confluence solenoid occupies CH1–4 (CH4 = central valve). Each tower then gets a contiguous 15-channel block at `base = 4 + index × 15`: 4-ch decoder followed by 11-ch uplight.

| Tower | Block | Decoder (set fixture to) | Fire valve | Uplight (set fixture to, 11-ch) |
|---|---|---|---|---|
| 0 | CH 5–19 | **A005** | CH8 | **A009** |
| 1 | CH 20–34 | **A020** | CH23 | **A024** |
| 2 | CH 35–49 | **A035** | CH38 | **A039** |
| 3 | CH 50–64 | **A050** | CH53 | **A054** |

`towerWrite()` (`towers.cpp`) writes both blocks from one `TowerState`. The web UI prints these addresses in each Tower Configs sub-tab and the Apply-to-All panel.

---

## Channel semantics (`towerWrite`)

**Decoder (base+1 … base+4):**
- base+1..3: strip R/G/B, scaled by `STRIP_BRIGHTNESS_PCT` (75%) to protect the old strips. Full white (all three channels) is the worst-case draw, so this ceiling bounds every theme.
- base+4: `state.fire` — the propane valve. Set only by the FSM (`flameLevel` during `FIRE_ACTIVE`), 0 otherwise. Never carries white.

**Uplight (base+5 … base+15):** full theme RGB on its R/G/B, `state.white` on the white dimmer (CH11), `masterDim=255`, `rgbStrobe=1`. White is independent of fire.

### `TowerState` (`towers.h`)

`wDim` is replaced by two fields:
- `uint8_t white` — uplight white channel (strobe CH11).
- `uint8_t fire` — decoder CH4 valve.

### Main loop (`Test_Button_DMX.ino`)

`themeRender()` runs every frame for colour + white. The FSM overlays:
- `FIRE_ACTIVE`: `state.fire = towerConfigs[i].flameLevel` (valve opens; colour/white untouched).
- `END_CUE`: white-flash fade applied to `state.white` (uplight), not the valve.
- IDLE/COOLDOWN: theme only.

So firing during a colour theme = colour + flames (no white); selecting `bright_white` = white uplight + no fire.

---

## Safety note

The boot-time visual diagnostic (`tests.cpp` `testDmxVisual`) previously set `wDim = 255`, which under this wiring is the fire valve — it pulsed all four valves for 500 ms on every boot. It now sets `white = 255, fire = 0`.

---

## Web UI

Per-tower and Apply-to-All panels show a `.dmx-addr` hint giving the decoder and uplight addresses. No new config field: **flame level** (existing per-tower slider) drives the fire valve; **white** comes from the white themes and the end-cue flash. The accumulator strips and uplight share one theme/brightness/speed config.

---

## Persistence

No new NVS fields. `flameLevel` already persists. `STRIP_BRIGHTNESS_PCT` is a compile-time constant in `towers.cpp`.

---

## Non-goals

- **A manual per-tower white slider.** "White on demand" is served by the white themes; a dedicated white level config could be added later (uplight CH11) if independent colour + white is wanted simultaneously.
- **Independent strobe vs. strip colour.** Both fixtures share one colour config.
- **Configurable strip cap via the UI.** Compile-time constant for now.
- **Auto-addressing fixtures over DMX/RDM.** Operators set fixture addresses by hand to the values above.
