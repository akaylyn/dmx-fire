# Spec: DMX transmitter quiet mode (bus handover)

## Context

Bench and field testing repeatedly needs a **second controller** on the bus — a manual
DMX console, or the Enttec driving `tools/dmx-tester/`. DMX512 has no arbitration: two
transmitters on one differential pair corrupt each other's frames, they do not merge. So
the only way to let another controller drive the fixtures has been to physically unplug
the M5, which is awkward in a rigged installation and loses the web UI at the same time.

Quiet mode stops the M5 emitting frames so the bus is free, without touching a cable.

It also closes a diagnostic gap. During the 2026-08-19 session (notes.md Session 5) the
decisive question was "is the controller actually sending bytes to this fixture?" — and
answering it meant comparing against a console. Being able to silence the M5 on demand
makes that a two-click test.

**This is necessarily global.** A DMX frame carries every slot `1..DMX_FRAME_SLOTS` every
time it is sent, so there is no such thing as going quiet for one tower. The per-tower
equivalent already exists: `TowerConfig.connected = false` keeps transmitting that tower's
channels as zero (`Test_Button_DMX.ino:209` skips `towerWrite()` for it).

---

## Safety

Stopping DMX makes every fixture **latch its last commanded value** — the same hazard
`docs/spec-ota-update.md` documents for uploads. A valve open at the moment frames stop
would stay open with nothing left running to close it.

Quiet mode therefore reuses OTA's two safety helpers, now shared (`ota.h`):

| Helper | Role |
|---|---|
| `rigSafeToStall(String& why)` | Refuses unless FSM is `IDLE`, no purge, no morse, audio disarmed. `why` is returned to the caller. |
| `rigForceEverythingClosed()` | Zeroes every channel and **transmits several real frames** so the zeros reach the wire — zeroing the shadow buffer alone is not enough. |

Order is fixed: **guard → force closed → go quiet.** `dmxUpdate()` also returns early
while quiet, so no other caller (`tests.cpp`, a future pacing loop) can break the silence.

---

## Technical details

**Flag:** `dmxSetQuiet(bool)` / `dmxQuiet()` in `dmx.h`/`dmx.cpp`. `dmxUpdate()` checks it
before the TX-drain guard, so a muted transmitter never touches the wire and never counts
a skipped frame.

**RAM-only, never persisted.** A saved "stop transmitting" flag would be the same trap as
a saved `connected=false`: it survives reboots and reads as dead hardware. A power cycle
always restores normal output. See notes.md Session 5.

**Endpoints** (`web.cpp`):

| Method | Path | Behaviour |
|---|---|---|
| POST | `/api/dmx/quiet/start` | `200 {"ok":true,"quiet":true}`, or `409 {"ok":false,"error":"<why>"}` if the rig is not idle |
| POST | `/api/dmx/quiet/stop` | `200 {"ok":true,"quiet":false}`. Idempotent. |

**`/api/state`** gains `dmx.quiet`, emitted alongside `dmx.ch`:

```json
"dmx": { "quiet": false, "ch": [ ... 64 bytes ... ] }
```

> `quiet` must be read **with** `ch`. The main loop keeps composing frames while muted, so
> `ch` is "what would be sent", not what is on the wire. The web UI labels it accordingly
> rather than claiming "on air".

---

## Web UI changes

A **Bus handover** fieldset at the top of the Tower Configs tab: explanatory copy, a live
readout (`data-live='bus'`), and a Go quiet / Resume transmitting pair that swap on state.
A refusal renders the server's reason inline. While quiet, every per-fixture readout drops
its "on air" line and says *"composed but not sent"* — see
[spec-live-fixture-state.md](spec-live-fixture-state.md).

Source of truth is `tools/web-preview/index.html`; ported to `buildPage()` via `/web-sync`.
`tools/web-preview/server.py` mocks both endpoints and the state field, including the 409.

---

## Persistence

**None, deliberately.** RAM-only; `false` on every boot. `tests/conftest.py` also clears it
in the shared baseline so a test that muted the transmitter and then died cannot leave the
rig dark for the rest of the session.

---

## Non-goals

- **No per-tower quiet.** Not expressible — a frame is all-or-nothing. Use `connected`.
- **No auto-expiry.** A timeout would resume transmitting mid-test and start fighting the
  console the operator handed the bus to. Visibility plus the boot reset is the mitigation.
- **No DMX receive.** The M5 goes silent; it does not listen. Monitoring a branch is the
  Enttec's job via `tools/dmx-tester/`.
- **Does not replace OTA's own guard.** OTA stalls the whole loop and keeps its own call to
  the shared helpers.
