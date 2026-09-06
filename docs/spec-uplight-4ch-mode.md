# Spec: Uplights in 4-channel mode

## Context

Each tower's **uplight** (LaluceNatz LL960S) was previously driven in **11-channel
mode**: master dimmer, RGB strobe speed, RGB effect + speed, R/G/B, white strobe
speed, white effect + speed, white dimmer. The firmware wrote all eleven channels
and used only four of them meaningfully.

In the field the uplights were found in **mixed modes** — some on 4 channels, some
on 8 — which is believed to be behind part of the "towers 1/2/3 not working"
symptom. All four have now been set to **4-channel mode**, and the firmware writes
a 4-channel block: **R, G, B, W**, plain linear dimming.

The switch is deliberately **address-neutral**: every fixture keeps the start
address it already has, so no re-addressing was needed on site.

Related: [spec-tower-fixtures-fire-white.md](spec-tower-fixtures-fire-white.md)
(fire/white separation, per-tower blocks),
[spec-confluence-addressing.md](spec-confluence-addressing.md) (central solenoid on
CH1), [hardware.md](hardware.md) (fixture list and addressing),
[notes.md](../notes.md) (field debug context).

---

## Channel map

Stride stays **15** channels per tower at `base = 4 + index × 15`, of which only
the first **8** are claimed: 4 decoder + 4 uplight.

**Per tower:**

| Offset | Fixture CH | Device | Signal | Value |
|---|---|---|---|---|
| +1 | 1 | Decoder | Strip Red | theme R × `STRIP_BRIGHTNESS_PCT` (75%) |
| +2 | 2 | Decoder | Strip Green | theme G × 75% |
| +3 | 3 | Decoder | Strip Blue | theme B × 75% |
| +4 | 4 | Decoder | **FIRE valve** | 255 during `FIRE_ACTIVE`/purge, else 0 (binary) |
| +5 | 1 | Uplight | Red | `state.ur` — theme R, or the fire look while a valve is open |
| +6 | 2 | Uplight | Green | `state.ug` |
| +7 | 3 | Uplight | Blue | `state.ub` |
| +8 | 4 | Uplight | **White** | `state.white` — white themes / fire look / end-cue flash |

> **Uplight RGB is no longer the same bytes as strip RGB.** `TowerState` gained
> separate `ur`/`ug`/`ub` fields so the uplight can hold a flame colour while the
> strips keep running the theme. `themeRender()` sets both to the same value, so
> idle output is byte-identical to before. See
> [spec-fire-uplight.md](spec-fire-uplight.md).
| +9 … +15 | — | — | *unclaimed* | 0, every frame |

**Fully resolved universe:**

| DMX CH | Fixture | Function |
|---|---|---|
| **1** | Confluence decoder | 🔥 **SOLENOID** |
| 2–4 | — | unclaimed / unwired (0) |
| **5–7** | T0 decoder **A005** | strip R / G / B (capped) |
| **8** | T0 decoder | 🔥 **FIRE valve** |
| **9–12** | T0 uplight **A009** | R / G / B / W |
| 13–19 | — | unclaimed (0) |
| **20–22** | T1 decoder **A020** | strip R / G / B (capped) |
| **23** | T1 decoder | 🔥 **FIRE valve** |
| **24–27** | T1 uplight **A024** | R / G / B / W |
| 28–34 | — | unclaimed (0) |
| **35–37** | T2 decoder **A035** | strip R / G / B (capped) |
| **38** | T2 decoder | 🔥 **FIRE valve** |
| **39–42** | T2 uplight **A039** | R / G / B / W |
| 43–49 | — | unclaimed (0) |
| **50–52** | T3 decoder **A050** | strip R / G / B (capped) |
| **53** | T3 decoder | 🔥 **FIRE valve** |
| **54–57** | T3 uplight **A054** | R / G / B / W |
| 58–64 | — | unclaimed (0) |

Valve channels: **1, 8, 23, 38, 53**. 36 of 64 channels are claimed; the other 28
are driven to 0.

### Set the fixtures to

| Fixture | Address | Mode |
|---|---|---|
| Accumulator decoders | A005 / A020 / A035 / A050 | 4-channel |
| Uplights | A009 / A024 / A039 / A054 | **4-channel** |
| Confluence decoder | A001 | 3-channel |

---

## Firmware

### `towers.h` — `TowerState` shrinks

Three fields were **removed**: `masterDim`, `rgbStrobe`, `wStrobe`. What remains:

```cpp
struct TowerState {
  uint8_t r, g, b;      // theme colour — accumulator strips (capped)
  uint8_t ur, ug, ub;   // uplight RGB (full) — added later, see spec-fire-uplight.md
  uint8_t white;        // uplight W — independent of fire
  uint8_t fire;         // decoder CH4 — propane valve
};
```

(`ur`/`ug`/`ub` were added after this spec, splitting uplight colour from strip
colour. They are plain colour fields and do **not** reopen the trap below.)

4-channel mode is plain per-colour linear dimming: there is **no master dimmer**
and **no strobe gate**, so there is nothing for those fields to drive. Brightness
is baked directly into `r`/`g`/`b`/`white` by `themeRender()` — which it already
was, since `masterDim` was hardcoded to 255 on every render and never modulated.
Dropping it changes no output.

### ⚠️ Do not reintroduce those fields

This is the trap worth remembering. In 11-channel mode the firmware wrote two
constants into the first two channels of the uplight block:

- `masterDim = 255` → block CH1
- `rgbStrobe = 1` → block CH2 (needed because on that fixture CH2 = 0 means "RGB
  section off"; 1–7 is steady/open, so without it the uplight showed no colour at
  all)

In **4-channel mode those same two slots are Red and Green.** Left in place, every
uplight would be pinned at **Red = 255, Green = 1** regardless of theme — full red
towers, permanently, with the real theme colour written past the end of the
fixture's span. The fields are gone from the struct so the mistake cannot be made
by editing `towers.cpp` alone.

### `themes.cpp`

`themeRender()` now returns a zero-initialised `TowerState` and sets only
`r`/`g`/`b`/`white`, with `brightness` already applied. The `rgbStrobe = 1`
workaround is deleted. Themes that blank (the `green`/`blue`/`fire` gradients, off
for 3200 ms of every 4000 ms cycle) return an all-zero state, which in 4-channel
mode is simply dark — no gate to keep open.

### `towers.cpp` — `towerWrite()`

Decoder block unchanged (capped strip RGB + `state.fire` on CH4). Uplight block is
now four writes — `state.r`, `state.g`, `state.b`, `state.white` — followed by a
loop that drives the rest of the stride to 0.

### `tests.cpp`

The boot visual diagnostic no longer sets `state.masterDim`; it sets
`r = g = b = white = 255` and `fire = 0`. The address-map printout reports each
tower's decoder, uplight, and unclaimed ranges, and reminds the operator that the
uplights must be in 4-channel mode.

---

## Why the 15-channel stride was kept

Only 8 channels per tower are claimed, so the stride could compact to 8 and the
universe would shrink from 64 to 36 channels. It was **not** compacted.

Compacting moves every fixture above Tower 0:

| | Current | Compacted (stride 8) |
|---|---|---|
| T0 decoder / uplight | A005 / A009 | A005 / A009 (unchanged) |
| T1 decoder / uplight | A020 / A024 | A013 / A017 |
| T2 decoder / uplight | A035 / A039 | A021 / A025 |
| T3 decoder / uplight | A050 / A054 | A029 / A033 |

That is **6 fixtures to re-address by hand**, on site, during an active
line-noise investigation, for no functional gain — the existing addresses are
already documented, printed in the web UI, and physically set. Keeping the stride
made the mode switch a firmware-only change.

The dead channels cost nothing: DMX frame length is fixed by the 64-channel
universe, not by how many channels carry meaning.

## Why unclaimed channels are driven to 0

No fixture listens on the 28 unclaimed channels, so their value is irrelevant to
any output — but they are written to 0 on every frame regardless.

An unwritten channel keeps whatever byte the buffer last held. Several of these sit
directly adjacent to valve channels on a bus that has shown noise and phantom
firing, so an explicitly-driven 0 removes a failure mode that would be miserable to
diagnose. Same reasoning as Confluence CH2–4 in
[spec-confluence-addressing.md](spec-confluence-addressing.md).

---

## The mixed-mode field bug

The uplights were found running in **different channel modes on different towers**
— some 4-channel, some 8-channel — while the controller sent one fixed layout.
Any fixture not in the mode the firmware assumes reads the wrong meaning off every
byte, which presents as per-tower behaviour that looks random and is easy to
mistake for a wiring or cabling fault.

8-channel mode is the nastiest case because **CH1 is a master dimmer**: the
firmware's Red value lands on the dimmer, R/G/B shift one channel up, and white
falls off the end of the block. The fixture responds to the bus, just never as
intended.

> **Operator check — setting the address does not set the mode.** All four
> uplights are LaluceNatz LL960S. On that fixture the DMX address and the channel
> mode are **independent settings**: the mode is its own menu item
> (`CH04` / `CH11` / `CH32` / `CH39`) and the factory default is **`CH11`**
> ([manual](manuals/strobe-lalucenatz-500w-rgbw.md)). The `A009`/`A024` notation
> used throughout these docs and in the web UI is shorthand for "DMX address 9 / 24"
> and says nothing about mode — each uplight needs **both** its address **and**
> `CH04` set explicitly, per tower.
>
> A unit left on `CH11` is the failure this spec exists to prevent: the firmware's
> Red lands on its master dimmer, Green on its strobe speed, and **Blue on its
> built-in-effect selector** (values 3–255 pick one of 84 internal patterns), while
> its real R/G/B channels sit in the stride tail that the firmware drives to 0. The
> visible result is the fixture running an internal chase — observed on site as a
> yellow pattern with a white runner — rather than showing theme colour at all.
>
> The 9PCS 4IN1 LED Washer is a **bench test light only and is not part of the
> installed rig**; its address-prefix mode selection (`d001` = 4-ch, `A001` = 8-ch)
> does not apply to any tower.

All four uplights are now in 4-channel mode. `tests.cpp`'s boot address map prints
the reminder, and the regression tests below fail loudly if the firmware and
fixtures disagree again.

---

## Web UI

The `.dmx-addr` hints now read **"Uplight (LaluceNatz 4ch R/G/B/W)"** in both the
per-tower panels and the Apply-to-All panel, replacing "LaluceNatz 11ch, colour +
white". Addresses shown are unchanged.

Source of truth is `tools/web-preview/index.html`; mirrored into `web.cpp`'s
`buildPage()` F-strings. No control changes — theme, brightness, speed and flame
level behave exactly as before.

`tools/web-preview/simulator.html` needed no change for the mode switch: it only ever
modelled R/G/B/W per fixture, never the strobe or dimmer channels. (It was updated
later for the uplight/strip colour split — see
[spec-fire-uplight.md](spec-fire-uplight.md).)

---

## Persistence

None added or removed by this spec. Per-tower `theme` / `bright` / `speed` / `fireEnabled` /
`connected` persist in NVS as before. The channel mode is a property of the
physical fixture plus `towerWrite()`, not a stored setting.

---

## Tests

Two new regression tests in `tests/test_dmx_output.py`:

- **`test_uplight_is_four_channel_rgbw`** — asserts each uplight block reads
  `R = G = B = W = 200` uncapped under `bright_white` at brightness 200, while the
  strip red on the same tower reads the capped 150. This is the direct guard
  against an 11-channel-style write returning: a stale `masterDim`/`rgbStrobe`
  would show up here as a bogus value on Red/Green. `bright_white` is used because
  it renders continuously, unlike the fire gradients.
- **`test_unclaimed_channels_stay_zero`** — asserts all 28 unclaimed channels
  (CH4, plus `base+9 … base+15` per tower) read 0 even with every theme and flame
  level driven to maximum.

Existing coverage still applies: `TOWER_FIRE_CH = [8, 23, 38, 53]` for the valves,
and `CONFLUENCE_FIRE_CH = 1`.

---

## Non-goals

- **No universe compaction.** The 15-channel stride and 64-channel universe stay,
  so no fixture needs re-addressing. Compacting to 36 channels is possible later
  (see the table above) but buys nothing on its own.
- **No strobe, effects, or sound-active support.** 4-channel mode physically
  cannot do it — no strobe, effect, or speed channels exist in that mode. Anything
  needing them would require putting the fixtures back into 11-channel mode (11
  channels per uplight, so the current stride already fits) and restoring the
  dimmer/strobe fields.
- **No master-dimmer path.** Brightness stays baked into RGB/white by
  `themeRender()`. This also keeps the accumulator strips and uplight consistent,
  since the strip decoder has no master dimmer either.
- **No per-segment control.** The LL960S's 32- and 39-channel modes expose its 8
  segments individually; the towers use it as a single flood.
- **No auto-detection of fixture channel mode.** Plain DMX512 is transmit-only
  with no feedback; operators set modes by hand and the boot diagnostic plus the
  tests above are the verification.
