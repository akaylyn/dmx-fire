# Spec: Per-tower fixtures, DMX addressing, fire/white separation

## Context

Each tower has **two DMX fixtures**, not one:

1. **Accumulator decoder** — a 4-channel decoder. CH1–3 drive the RGB LED strips wrapped around the accumulator tank; **CH4 is the propane fire valve only**. The strips are RGB-only, old, and power-limited (sending full R+G+B for white overdraws the supply), so their colour is capped.
2. **Uplight** — an RGBW fixture in **4-channel mode** (R/G/B/W linear dimming). Full theme colour on its RGB channels plus a **dedicated white channel** (CH4 of its block) that is safe to drive. Originally specced in 11-channel mode; see [spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md).

Two problems this spec fixes:

- **Fire forced white.** Previously the per-tower CH4 carried "white" (`wDim`), which was both the strip-decoder's W and the valve — firing lit everything white, and the boot diagnostic pulsed all valves. Fire and white are now separate channels.
- **All towers showed the same colour.** `towerWrite` wrote every tower to the same base address, so per-tower theme rendering (e.g. `simon`) wasn't visible. Each tower now has its own address block.

Goal: **fire without white, white without fire**, and per-tower independent colour.

---

## DMX address layout

Central confluence solenoid occupies CH1–4 (**CH1 = central valve**; 3-channel decoder at A001). Each tower then gets a 15-channel stride at `base = 4 + index × 15`: a 4-ch decoder followed by a 4-ch uplight, leaving 7 unclaimed channels driven to 0.

| Tower | Stride | Decoder (set fixture to) | Fire valve | Uplight (set fixture to, 4-ch) | Unclaimed |
|---|---|---|---|---|---|
| 0 | CH 5–19 | **A005** | CH8 | **A009** (CH 9–12) | CH 13–19 |
| 1 | CH 20–34 | **A020** | CH23 | **A024** (CH 24–27) | CH 28–34 |
| 2 | CH 35–49 | **A035** | CH38 | **A039** (CH 39–42) | CH 43–49 |
| 3 | CH 50–64 | **A050** | CH53 | **A054** (CH 54–57) | CH 58–64 |

`towerWrite()` (`towers.cpp`) writes both blocks from one `TowerState`. The web UI prints these addresses in each Tower Configs sub-tab and the Apply-to-All panel.

---

## Channel semantics (`towerWrite`)

**Decoder (base+1 … base+4):**
- base+1..3: strip R/G/B, scaled by `STRIP_BRIGHTNESS_PCT` (75%) to protect the old strips. Full white (all three channels) is the worst-case draw, so this ceiling bounds every theme.
- base+4: `state.fireOpen` — the propane valve. Opened only by the FSM (`FIRE_ACTIVE`) or purge, and only ever to 255. Never carries white. See spec-solenoid-binary.md.

**Uplight (base+5 … base+8):** full theme RGB on its R/G/B and `state.white` on its W, in 4-channel mode. White is independent of fire. Channels base+9 … base+15 are unclaimed and driven to 0. See [spec-uplight-4ch-mode.md](spec-uplight-4ch-mode.md).

### `TowerState` (`towers.h`)

`wDim` is replaced by two fields:
- `uint8_t white` — uplight white channel (strobe CH11).
- `uint8_t fire` — decoder CH4 valve.

### Main loop (`Test_Button_DMX.ino`)

`themeRender()` runs every frame for colour + white. The FSM overlays:
- `FIRE_ACTIVE`: `state.fireOpen = mgOn && towerConfigs[i].fireEnabled` (valve opens fully) **and the uplight takes the configured fire look** — see below.
- `END_CUE`: white-flash fade applied to `state.white` (uplight), not the valve. Scaled to `buttonConfig.endCueMs`.
- IDLE/COOLDOWN: theme only.

> **Superseded in part by [spec-fire-uplight.md](spec-fire-uplight.md).** The original
> rule here was that firing left "colour/white untouched", so the uplight stayed
> purely theme-driven while the valve was open. That is no longer true: while any
> valve is open the uplight holds a configurable warm flame colour, and `TowerState`
> gained separate `ur`/`ug`/`ub` fields so the uplight can move independently of the
> accumulator strips. What has **not** changed is the channel separation this spec
> exists to document — fire is decoder CH4, white is uplight CH4, and neither drives
> the other. The **accumulator strips are still purely theme-driven in every state.**

So firing during a colour theme = flame-coloured uplight + animated strips + flames; selecting `bright_white` = white uplight + no fire.

---

## Safety note

The boot-time visual diagnostic (`tests.cpp` `testDmxVisual`) previously set `wDim = 255`, which under this wiring is the fire valve — it pulsed all four valves for 500 ms on every boot. It now sets `white = 255, fire = 0`.

---

## Web UI

Per-tower and Apply-to-All panels show a `.dmx-addr` hint giving the decoder and uplight addresses. No new config field: **flame level** (existing per-tower slider) drives the fire valve; **white** comes from the white themes and the end-cue flash. The accumulator strips and uplight share one theme/brightness/speed config.

---

## Persistence

No new NVS fields for this spec. The per-tower propane flag is `fireEnabled` (`t<N>v`) — see spec-solenoid-binary.md. `STRIP_BRIGHTNESS_PCT` is a compile-time constant in `towers.cpp`.

---

## Non-goals

- **A manual per-tower white slider.** "White on demand" is served by the white themes. A *global* white level for the fire look was since added — see [spec-fire-uplight.md](spec-fire-uplight.md) — but there is still no per-tower white control.
- **Independent strobe vs. strip colour.** Both fixtures share one colour config.
- **Configurable strip cap via the UI.** Compile-time constant for now.
- **Auto-addressing fixtures over DMX/RDM.** Operators set fixture addresses by hand to the values above.
