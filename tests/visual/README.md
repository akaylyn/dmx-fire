# Visual hardware tests

Drives the DMX rig over the API while a camera watches the fixtures, then
emits an annotated report so you can review which tests passed/failed and
look at the corresponding camera frames.

## Layout

```
tests/visual/
├── scripts/
│   ├── flash.sh           # arduino-cli compile + upload (with retry)
│   ├── check_network.sh   # verify dmx-fire AP is reachable
│   ├── visual_test.py     # the test runner
│   └── run.sh             # wrapper: optional flash, network check, run tests
└── runs/
    └── <UTC timestamp>[_label]/
        ├── *.jpg          # camera captures, one per step
        ├── report.json    # machine-readable
        └── report.md      # open this — has pass/fail + image refs
```

## Prerequisites

```sh
# One-time: create venv with deps
python3 -m venv .venv
.venv/bin/pip install -r tests/requirements.txt pyserial

# One-time: install camera + JSON tools (Homebrew)
brew install imagesnap

# Each session: laptop must be joined to the dmx-fire AP
```

## Run

```sh
# Test against currently-flashed firmware
tests/visual/scripts/run.sh

# Compile, upload, then test
tests/visual/scripts/run.sh --flash

# Tag the run for easier review later
tests/visual/scripts/run.sh --label idle-flash-tweak
```

Each invocation creates a NEW directory under `runs/` — earlier runs are
kept untouched so you can compare behaviour across firmware iterations.

## What's tested

| ID  | Name                                              | Pass criterion |
|-----|---------------------------------------------------|----------------|
| T01 | GET /api/state shape                              | All top-level keys present, `towers` len 4, `dmx.ch` len 64 |
| T02 | IDLE flash visible                                | At least one frame in the burst has tower-0 RGBW > 0 |
| T03 | IDLE blank visible                                | At least one frame has tower-0 RGBW all = 0 |
| T04 | FIRE_ACTIVE drives towers                         | `fsm.state == FIRE_ACTIVE` and tower-0 RGBW > 0 after press |
| T05 | FSM walks FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE | All four states observed |
| T06 | PARTY mode release ends fire early                | `pre=FIRE_ACTIVE`, `post=END_CUE/COOLDOWN/IDLE` |
| T07 | Tower disconnect via `/set`                       | `connected` flips true → false |

T01, T04–T07 are functional checks that don't depend on the camera. T02 and
T03 confirm the IDLE flash behaviour by polling DMX shadow values across a
4-second window.

## Why nc and not requests

The host laptop has a macOS network-extension quirk where Python sockets
return `EHOSTUNREACH` on `192.168.4.1` even though `ping` and `nc` both work.
The runner uses `nc` for HTTP via `subprocess` to side-step that. If your
machine doesn't have the same issue, swap `http()` in `visual_test.py` for
`requests.get/post` — the rest of the runner is independent.

## Troubleshooting

- **`PASS: 192.168.4.1 reachable`** but everything fails — laptop is on the
  AP but the device is busy. Try `arduino-cli` reflash or pull the USB.
- **`A fatal error occurred: chip stopped responding`** — the M5 USB-serial
  is flaky after a reset. `flash.sh` retries up to 3×; if it still fails,
  unplug and replug the device.
- **Camera images all black** — imagesnap needs warmup ≥0.8s between
  captures; the runner uses 0.9s by default.
