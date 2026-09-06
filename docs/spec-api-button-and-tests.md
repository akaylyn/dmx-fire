# Spec: API-Triggered Button & Pytest Test Harness

## Context

Today the only way to fire the rig is the physical button on GPIO 39. There is no programmatic trigger and no host-side test suite — `tests.cpp` runs informational diagnostics on-device and prints `[PASS]`/`[FAIL]` to Serial, but nothing asserts on real device state from a workstation.

This spec adds:

1. **API-triggerable button press**, mirrored in the web UI as a "Test Fire" button. Same FSM path as the physical button so behaviour is identical.
2. **Host-side pytest harness** that drives the ESP32 over WiFi via the existing/new HTTP endpoints, configures the system, simulates presses, and asserts on returned state. Covers both existing and new functionality.

The new endpoints are also used internally by the new UI button, so the test surface and the operator UX share one path.

---

## New HTTP endpoints (`web.cpp`)

| Method | Path | Body | Effect | Response |
|---|---|---|---|---|
| POST | `/api/button/press` | none | Sets one-shot virtual `wasPressed=true` and sticky `virtualHeld=true` | `200` empty |
| POST | `/api/button/release` | none | Sets one-shot virtual `wasReleased=true` and clears `virtualHeld` | `200` empty |
| POST | `/api/button/reset` | none | Forces FSM back to `FSM_IDLE` (skip cooldown for tests) | `200` empty |
| GET  | `/api/state` | — | Full system snapshot as JSON | `200 application/json` |

The existing `POST /set` form-urlencoded endpoint stays as the configuration mutation path — it's already exhaustive and Python `requests` handles form encoding trivially. No new config endpoint needed.

### `/api/state` JSON schema

```json
{
  "uptime_ms": 12345,
  "fsm": { "state": "IDLE", "elapsed_ms": 234 },
  "button": { "mode": 0, "fireDurationMs": 3000, "cooldownMs": 10000, "endCuePattern": 0 },
  "confluence": { "connected": true, "fireEnabled": true },
  "towers": [
    { "connected": true, "fireEnabled": true, "theme": "green", "brightness": 128, "speed": 100 }
  ],
  "dmx": { "ch": [0, 0, 0, 255, ...] }
}
```

`fsm.state` is one of `"IDLE" | "FIRE_ACTIVE" | "END_CUE" | "COOLDOWN"`. `dmx.ch` is 64 bytes mirroring the universe (index 0 = DMX channel 1). JSON is hand-rolled with `String` to avoid adding ArduinoJson as a dep — payload shape is fixed.

---

## Virtual-button injection (`button_fsm.h/.cpp`, `Test_Button_DMX.ino`)

Add to `button_fsm.h`:

```cpp
void buttonInjectPress();    // sets pendingPress + held=true
void buttonInjectRelease();  // sets pendingRelease + held=false
void buttonInjectReset();    // forces FSM_IDLE
bool buttonConsumePress();   // returns + clears pendingPress
bool buttonConsumeRelease(); // returns + clears pendingRelease
bool buttonVirtualHeld();    // returns sticky held flag
```

In `Test_Button_DMX.ino` loop, OR the physical events with virtual ones:

```cpp
bool btnPressed  = keyButton.wasPressed()  || buttonConsumePress();
bool btnReleased = keyButton.wasReleased() || buttonConsumeRelease();
bool btnHeld     = keyButton.isPressed()   || buttonVirtualHeld();
buttonFsmTick(btnPressed, btnReleased, btnHeld);
```

`buttonInjectReset()` directly forces `fsmState = FSM_IDLE` and resets `stateEnteredMs`. Physical and API events feed the same `buttonFsmTick()` so all existing FSM behaviour (FIREBALL vs PARTY, cooldown, end-cue fade) applies identically.

---

## DMX last-frame snapshot (`dmx.h/.cpp`, `towers.cpp`, `confluence.cpp`)

For tests to assert on DMX output, expose what was last sent.

Add to `dmx.h`:

```cpp
extern uint8_t dmxLastFrame[64];   // mirrors the universe; index 0 = ch 1
void dmxShadowWrite(uint8_t value, uint16_t ch);
```

`dmxShadowWrite(v, ch)` updates `dmxLastFrame[ch-1]` and calls `dmxDevice.writeByte(v, ch)`. Replace direct `dmxDevice.writeByte(...)` calls in `towers.cpp` and `confluence.cpp` with `dmxShadowWrite(...)`. The shadow stays in sync with the wire.

`/api/state` reads `dmxLastFrame` into the `dmx.ch` array.

---

## Web UI: "Test Fire" button

Add a fieldset to the top of `buildPage()`:

```html
<fieldset><legend>Test Fire</legend>
  <button type="button" id="testFireBtn">Press &amp; hold to fire</button>
</fieldset>
```

JS uses `mousedown`/`touchstart` → `POST /api/button/press` and `mouseup`/`mouseleave`/`touchend` → `POST /api/button/release`. Press-and-hold semantics let the operator exercise PARTY-mode early-release behaviour from the UI exactly like the physical button.

---

## Pytest harness (`tests/`)

```
tests/
  README.md          how to run; assumes ESP32 reachable on WiFi
  requirements.txt   pytest, requests
  conftest.py        device fixture + autouse baseline reset
  api.py             Client wrapping all endpoints
  test_state.py
  test_config_per_tower.py
  test_config_all_towers.py
  test_config_confluence.py
  test_config_button.py
  test_storage.py
  test_fsm_transitions.py
  test_fireball_mode.py
  test_party_mode.py
  test_cooldown_lockout.py
  test_dmx_output.py
```

### Fixture details

- `device` (session scope): `api.Client(host)` — host from `DMXFIRE_HOST` env var (default `http://192.168.4.1`).
- `baseline` (autouse): before each test, calls `device.reset()` then `device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000)` so timing-sensitive tests run in ~3 s instead of ~14 s. Restores tower/confluence to known defaults.

### Run

```
pip install -r tests/requirements.txt
export DMXFIRE_HOST=http://192.168.4.1
pytest tests/ -v
```

The harness needs to be on the same WiFi as the AP (or the laptop joined to the ESP32's AP).

---

## Files Modified

| File | Change |
|---|---|
| `Test_Button_DMX/button_fsm.h` | Add inject/consume helpers |
| `Test_Button_DMX/button_fsm.cpp` | Implement helpers; expose state-reset path |
| `Test_Button_DMX/Test_Button_DMX.ino` | OR physical events with virtual events in loop |
| `Test_Button_DMX/dmx.h` | Declare `dmxLastFrame` + `dmxShadowWrite` |
| `Test_Button_DMX/dmx.cpp` | Define shadow buffer + write fn |
| `Test_Button_DMX/towers.cpp` | Route writes through `dmxShadowWrite` |
| `Test_Button_DMX/confluence.cpp` | Route writes through `dmxShadowWrite` |
| `Test_Button_DMX/web.cpp` | Add 4 new handlers; register routes; add Test Fire fieldset to `buildPage()` |

## Files Created

| File | Purpose |
|---|---|
| `tests/README.md` | How to run |
| `tests/requirements.txt` | `pytest`, `requests` |
| `tests/conftest.py` | Fixtures + baseline reset |
| `tests/api.py` | HTTP client wrapper |
| `tests/test_*.py` | 11 test files |

---

## Verification

1. `arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX` — compiles clean.
2. Upload and connect to `dmx-fire` AP → open `http://192.168.4.1/`. Test Fire button appears and physical button still fires.
3. Curl smoke:
   ```
   curl -X POST http://192.168.4.1/api/button/press
   curl http://192.168.4.1/api/state | jq '.fsm'
   curl -X POST http://192.168.4.1/api/button/release
   curl -X POST http://192.168.4.1/api/button/reset
   ```
4. `DMXFIRE_HOST=http://192.168.4.1 pytest tests/ -v` — all 11 test files pass against live hardware.
5. Physical button regression — physical press still triggers fire identically (virtual injection must not break the OR path).
