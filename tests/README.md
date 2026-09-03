# DMX-Fire Pytest Harness

Host-side integration tests that drive the ESP32 over WiFi via HTTP.
Covers configuration, FSM transitions, button modes, persistence, and DMX output.

## Prerequisites

1. Firmware built from `Test_Button_DMX/` flashed to the ESP32.
2. ESP32 powered on, broadcasting its AP (default SSID `dmx-fire`, see `secrets.h`).
3. Workstation joined to the same AP — laptop must be on the device's WiFi.

## Setup

```sh
python3 -m venv .venv
source .venv/bin/activate
pip install -r tests/requirements.txt
```

## Run

Default host is `http://192.168.4.1` (ESP32 AP IP):

```sh
pytest tests/ -v
```

Override the host:

```sh
DMXFIRE_HOST=http://10.0.0.42 pytest tests/ -v
```

Run a single file:

```sh
pytest tests/test_party_mode.py -v
```

## What's covered

| File | What it tests |
|---|---|
| `test_state.py` | `/api/state` shape, defaults, uptime |
| `test_config_per_tower.py` | Per-tower theme / brightness / speed / fireEnabled / connected round-trip |
| `test_config_all_towers.py` | `target=all` applies to every tower |
| `test_config_confluence.py` | Confluence connected + fireEnabled toggles |
| `test_config_button.py` | mode / fireDurationMs / cooldownMs / endCuePattern |
| `test_storage.py` | Config writes survive read-back via `/api/state` |
| `test_fsm_transitions.py` | IDLE → FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE via API |
| `test_fireball_mode.py` | mode=0 — release before duration: fire continues |
| `test_party_mode.py` | mode=1 — release before duration: fire ends early |
| `test_cooldown_lockout.py` | Press during COOLDOWN is ignored |
| `test_dmx_output.py` | DMX channels reflect config (connected, fireEnabled) |
| `test_valve_binary.py` | Valve channels are 0 or 255 only, under every fire source |

## Notes

- The autouse `baseline` fixture in `conftest.py` resets the FSM to IDLE and shortens
  `fireDurationMs`/`cooldownMs` before each test so the suite runs in ~30–60 s.
- Tests assume a real ESP32 — there is no mocked or simulated mode. CI integration
  would need a hardware-in-the-loop runner.
- A reboot cycle test for NVS persistence is intentionally omitted: the harness
  cannot power-cycle the device. `test_storage.py` verifies that writes are
  visible via `/api/state`, which is the persistence path the firmware uses.
