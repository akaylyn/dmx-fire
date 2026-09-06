# Spec: Binary Solenoid Channels

## Context

Every propane valve on the rig used to be driven by an operator-settable byte:

| Setting | Scope | NVS key | Web UI |
|---|---|---|---|
| `TowerConfig.flameLevel` | per tower | `t<N>f` | "Flame level" slider, 0–255 |
| `ConfluenceConfig.fireLevel` | central solenoid | `cffl` | "Fire level" slider, 0–255 |

That byte reached DMX CH 1 / 8 / 23 / 38 / 53 **verbatim** — no scaling existed
anywhere in the path. So the slider never made a smaller flame. All it decided
was whether the decoder's turn-on threshold was cleared. Below that threshold
the valve stayed shut or the coil chattered. Flame size is set by gas pressure
and orifice, not by DMX.

The codebase already knew this and had been papering over it for a long time:

- `towers.h` documented the field as "NOT a proportional flame control".
- The web UI's live readout warned on any value under 128.
- `docs/spec-live-fixture-state.md` carried a standing correction.
- `notes.md` records a field session lost to `flameLevel` before the real cause
  (a missing ground reference) was found.
- `scripts/towers.sh` exists partly to flag `flameLevel=0`.

A control that every layer has to warn about should not exist. This spec removes
it, and replaces the warnings with a structural guarantee.

## The rule

**A valve channel carries `0` or `255`. Nothing else can be written to one.**

This is enforced in three independent places, so it cannot be lost to a refactor:

1. **Types.** A partial value is unrepresentable. `TowerState.fire` (`uint8_t`)
   became `TowerState.fireOpen` (`bool`); `confluenceWrite(uint8_t level)` became
   `confluenceWrite(bool open)`; `morseTick()` returns `bool`.
2. **The DMX write.** `dmxShadowWrite()` refuses any byte other than 0 or 255 on
   a registered valve channel, and logs it at ERROR.
3. **The UI and API.** There is no field to set. `/set` accepts no level, and
   `/api/state` reports none.

## Valve channel registry (`dmx.h` / `dmx.cpp`)

Valve-ness used to be implicit — a `base + 4` offset convention plus the literal
`1` — and the same five numbers were hand-copied into the DMX log line, three
test modules, `scripts/towers.sh` and two browser tools. Nothing could ask "is
this channel a valve?", so nothing could enforce anything about one.

```cpp
static const uint8_t  NUM_VALVE_CHANNELS = 5;
extern const uint16_t VALVE_CHANNELS[NUM_VALVE_CHANNELS];  // {1, 8, 23, 38, 53}

static const uint8_t VALVE_CLOSED = 0;
static const uint8_t VALVE_OPEN   = 255;

bool dmxIsValveChannel(uint16_t ch);
void dmxValveWrite(uint16_t ch, bool open);   // the ONLY way to command a solenoid
```

`dmxShadowWrite()` gains the guard:

```cpp
if (dmxIsValveChannel(ch) && value != VALVE_CLOSED && value != VALVE_OPEN) {
  LOG_E("[DMX] refused %u on valve CH%u - solenoid channels are 0 or 255 only", value, ch);
  return;
}
```

**Refuse, not clamp.** A caller that computed 128 for a solenoid has a bug, and
guessing which way to round it is guessing about propane. Dropping the write
leaves the channel at whatever it last held, and every path that opens a valve
rewrites it every frame, so the safe steady state is already on the wire.

`towers.cpp` exposes `towerValveChannel(index)` so callers can close a valve
without composing a whole `TowerState`; the 15-channel stride math stays private.

## `fireEnabled` replaces the level

Per tower and for the Confluence, a boolean:

```cpp
struct TowerConfig      { bool connected; bool fireEnabled; /* ... */ };
struct ConfluenceConfig { bool connected; bool fireEnabled; };
```

`connected` was too blunt to reuse for this. Unticking it blanks the whole
fixture, which on the wire is indistinguishable from a dead decoder or a broken
cable — a failure mode that already cost one field session. `fireEnabled`
isolates *only* the propane: the lights keep running, so an isolated tower still
looks alive on stage.

It gates every source alike — button, purge, Morse, audio — so switching it off
isolates that fixture from all of them in one place.

## Related fix: the latched-valve hazard

A fixture marked `connected=false` was skipped in the frame loop, and **a DMX
channel that stops being written keeps its last byte.** Unticking "Connected" on
a tower mid-burn therefore left its solenoid latched at 255 with nothing left to
close it.

Now that valve channels are addressable data, both skip paths close the valve
explicitly before skipping:

```cpp
if (!towerConfigs[i].connected) {
  dmxValveWrite(towerValveChannel(i), false);
  continue;
}
...
} else {
  confluenceWrite(false);   // disconnected Confluence
}
```

The lighting channels still go dark, as before — only the valve is forced.

## Web UI

- **Confluence tab:** the "Fire level" slider is replaced by a "Fire enabled"
  checkbox.
- **Towers tab:** all five "Flame level" sliders are gone. Each per-tower panel
  gains a "Fire enabled" checkbox next to "Connected".
- **Apply to All** deliberately carries **no** `fireEnabled`, for the same reason
  it carries no `connected`: a browser submits nothing for an unchecked box, so
  one Apply-to-All from a form without that box would clear the flag on all four
  towers at once.
- **Live readout:** the `flameLevel === 0` / `< 128` and `fireLevel === 0` /
  `< 128` warnings are deleted outright. In their place, a `fireEnabled === false`
  notice, plus `vf()` — which flags any valve byte that is neither 0 nor 255.
  That should now be impossible, so it reads as a firmware fault, not a setting.

## HTTP API

`POST /set`:

| target | valve field |
|---|---|
| `confluence` | `fireEnabled` (checkbox; absent = off) |
| `0`–`3` | `fireEnabled` (checkbox; absent = off) |
| `all` | none |

A legacy client still posting `flameLevel=` or `fireLevel=` gets a 200 and no
effect. The request is not rejected: failing an operator's POST mid-show to
punish a stale bookmark is the wrong trade.

`GET /api/state`:

```json
"confluence": { "connected": true, "fireEnabled": true },
"towers": [ { "connected": true, "fireEnabled": true,
              "theme": "green", "brightness": 128, "speed": 100 } ]
```

`flameLevel` and `fireLevel` are gone from the payload entirely.

## Persistence

| Key | Type | Default | Note |
|---|---|---|---|
| `t<N>v` | bool | `true` | per-tower `fireEnabled` |
| `cffe` | bool | `true` | Confluence `fireEnabled` |

The retired `t<N>f` and `cffl` keys are **not** reused. They hold `UChar`
entries, and `Preferences::getBool()` on a type-mismatched key silently returns
the default — too quiet a behaviour to rest a propane setting on. `storageSave()`
calls `prefs.remove()` on both, so a rig that has run the old firmware sheds them
on its first config write. No `scripts/flash.sh --erase` needed.

## Tests

**On-device** (`tests.cpp`, printed by `runDiagnostics()` at boot):

- `testValveChannelMap()` — every tower valve derived from the stride appears in
  `VALVE_CHANNELS`, CH1 is registered, and no colour channel is. This is what
  keeps `towers.cpp`'s stride and `dmx.cpp`'s list from drifting apart.
- `testValveGuardRefusesPartial()` — writes 1/64/127/128/200/254 to a live valve
  channel and asserts none of them land; then checks both legal values do, and
  that a neighbouring colour channel still accepts 128.

**Host** (`tests/test_valve_binary.py`, using the shared map in `tests/valves.py`):

every valve byte is 0 or 255 during FIRE_ACTIVE, purge, MACHINE_GUN, Morse and a
full FSM cycle; MACHINE_GUN pulses between *exactly* those two values; a legacy
`flameLevel=` post is inert; `fireEnabled=false` shuts the valve while the
uplight still shows the fire look; disconnecting a fixture mid-burn drives its
valve to 0; and a non-valve channel still carries a mid-scale byte.

## Non-goals

- **No proportional flame control, ever.** Not per tower, not global, not
  per-shot. The hardware cannot express it. If a future rig gets a proportional
  gas valve, that is a new fixture type with its own spec — not a level byte on
  a solenoid channel.
- **No purge level.** Purge is fully open or not purging.
- **No intensity ramp in MACHINE_GUN.** The burst slider gates *time*, not
  amplitude.
- **No change to lights.** Uplight white, the fire-look colour, `STRIP_BRIGHTNESS_PCT`
  and every RGB channel stay fully dimmable. The guard is scoped to the five
  valve channels and must never widen.
- **No change to FSM timing** — `fireDurationMs`, `cooldownMs`, `endCueMs` and
  the audio limiter are untouched. This changes the amplitude rule only.
