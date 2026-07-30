# Spec: Empty Accumulator (Purge)

## Context

The normal fire path (physical button or Test Fire) is time-bounded: `FSM_FIRE_ACTIVE`
runs at most `fireDurationMs` and is followed by an `END_CUE` and a `COOLDOWN`
lockout. That is correct for effects, but there is no way to simply **hold every
propane valve open** to bleed the accumulators down — e.g. purging the lines at
end of night, or clearing pressure during setup/teardown.

This spec adds a dedicated **Empty Accumulator** control: a press-and-hold button
that opens **all four tower accumulator valves and the central Confluence
solenoid** for exactly as long as it is held — **no duration limit, no cooldown**.
It is gated behind the same arm/cover safety toggle as Test Fire.

The purge path is intentionally **independent of the FSM**. It is a per-frame
overlay evaluated in the main DMX loop, so it cannot be blocked by an in-progress
cooldown and never itself triggers one.

---

## Firmware

### Purge overlay (`button_fsm.h/.cpp`)

A single RAM flag (`g_purgeActive`, not persisted) with three accessors:

| Function | Effect |
|---|---|
| `purgeStart()` | sets the flag — valves held open |
| `purgeStop()`  | clears the flag — valves close |
| `purgeActive()`| current flag state |

### Main loop (`Test_Button_DMX.ino`)

Inside the 50 Hz DMX block, `purgeActive()` is read once per frame as `purge`:

- **Towers:** after the FSM switch, `if (purge) state.fire = towerConfigs[i].flameLevel;`
  — purge overrides whatever the FSM set, opening each tower's decoder CH4 valve.
  Colour/white are left as the theme rendered them (firing never forces white).
- **Confluence:** `purge` is the highest-priority source for CH1, above Morse and
  `FSM_FIRE_ACTIVE`: `if (purge) cfLevel = confluenceConfig.fireLevel;`.

Disconnected towers/confluence are skipped exactly as in the normal path.

### HTTP endpoints (`web.cpp`)

| Method | Path | Effect | Response |
|---|---|---|---|
| POST | `/api/purge/start` | `purgeStart()` — valves open while held | `200` empty |
| POST | `/api/purge/stop`  | `purgeStop()` — valves close | `200` empty |

`/api/state` gains a top-level `"purge": true|false` field so tests and the UI can
observe the overlay.

---

## Web UI

New top-level tab **"Empty Accum."** (`data-tab='purge'`), placed second in the tab
bar (after Test Fire). Its panel reuses the Test Fire layout:

- An **arm/cover row** (`purgeArmToggle` / `purgeArmRow` / `purgeArmState`) that
  resets to **Safe** on every page load and must be opened before the button reacts.
- A large press-and-hold **`#purgeBtn`** styled like the fire button but **amber**
  (`#d67d00`) instead of red, so the two hold-to-act controls are not confused.
  Reads `PRESS & HOLD / TO EMPTY` when armed, `DISARMED` otherwise.
- Hint text spelling out that it opens every tower valve and the Confluence
  solenoid with no time limit and no cooldown.

### Shared hold-button JS

The Test Fire and Purge buttons are now driven by one `setupHold(opts)` helper
(press/hold → `pressUrl`, release/leave/disarm → `releaseUrl`, arm state persisted
in `localStorage` fingerprinted by the device `boot_id`). Test Fire uses
`/api/button/press|release` with key `dmxFireArm`; Purge uses
`/api/purge/start|stop` with key `dmxFirePurgeArm`. Each button keeps its own arm
state; a single `/api/state` fetch resolves `boot_id` and restores both.

Source of truth is `tools/web-preview/index.html`; the same markup/CSS/JS is
mirrored into `web.cpp`'s `buildPage()` F-strings.

---

## Persistence

None. `g_purgeActive` is RAM-only and defaults to `false` at boot. The arm/cover
state is browser-side `localStorage` only, invalidated on every device reboot via
`boot_id` (same mechanism as Test Fire).

---

## Non-goals

- **No physical-button purge.** Purge is web-UI/API only; the GPIO39 button keeps
  its existing FSM behaviour.
- **No per-tower purge selection.** Purge always opens all connected towers plus
  Confluence. Per-fixture isolation stays in the Tower Configs "Connected" toggles.
- **No purge level control.** Purge uses each fixture's existing `flameLevel` /
  Confluence `fireLevel`; it does not add its own level slider.
- **No auto-timeout / dead-man logic beyond button release.** Releasing the button
  (or disarming, or losing the page) sends `/api/purge/stop`; there is no separate
  server-side maximum-open timer.
