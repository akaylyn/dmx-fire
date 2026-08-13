# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Commands

**Compile firmware:**
```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX
```

**Upload to device (flash):**
```bash
scripts/flash.sh
scripts/flash.sh --erase   # full wipe, use if device is in a boot loop
```

> **Never run two flash instances at the same time.** Running a second flash while one is in progress — or killing a running flash with `pkill` and immediately starting another — corrupts the bootloader and forces a manual recovery. Wait for the current flash to finish or abort it cleanly with CTRL-C before starting a new one.

### Flashing the M5AtomS3 (ESP32-S3)

**Turn off Bluetooth before uploading.** macOS Bluetooth device enumeration shares USB bus resources and can interfere with the USB-Serial/JTAG connection mid-flash. Disable Bluetooth in System Settings or the menu bar before running `scripts/flash.sh`.

**USB cable:** Must be a data-capable cable, not a charge-only cable. Charge-only cables have only 2 wires (power) and will never establish a serial connection — the device LED will light up but esptool will time out. Test with a cable you know carries data.

**Bootloader mode:** The ESP32-S3 uses a USB-Serial/JTAG controller that can't always auto-reset into bootloader mode (unlike older ESP32 which used RTS/DTR). If the flash script fails repeatedly, enter bootloader manually:
1. Hold the side button (GPIO0 low)
2. Unplug and replug USB (or tap the reset button if available) while holding
3. Release the button — device is now in download mode
4. Run `scripts/flash.sh` immediately

**Why block-by-block writes:** The M5AtomS3 uses an ESP32-S3 rev 0.2 which has a hardware errata where the USB-Serial/JTAG controller conflicts with the flash controller during 64 KB block erases. Writing all files in a single esptool session triggers multiple 64 KB erases back-to-back and drops the USB connection partway through. The fix (already in flash.sh) is one 64 KB block per esptool connection. Each block takes ~20 s; a full 1.1 MB firmware takes ~5 minutes but completes reliably. **Do not revert to a single-session write.**

**Browser-based flash recovery (automatic failover):** When any 64 KB block fails after `scripts/flash.sh`'s 12 retries, the script now auto-launches a browser recovery tool. If the internet is reachable it opens https://espressif.github.io/esptool-js/; otherwise it serves a local copy from `tools/recovery/` at `http://localhost:8765/` via `python3 -m http.server`. The terminal prints the four `.bin` paths and offsets the operator should drop into the browser UI, along with the button-hold ROM-bootloader steps. The browser flow uses Web Serial (Chrome/Edge), which often succeeds when the CLI's macOS USB path is wedged. After a successful browser flash, no further CLI run is needed; for a partial recovery (e.g. erase only) re-run `scripts/flash.sh`. See `docs/spec-flash-recovery-failover.md` for details.

**Boot loop recovery:** If the device is stuck in a boot loop with no firmware ("waiting for download" on the serial console), run `scripts/flash.sh`. The block-by-block write rewrites every sector, so `--erase` is only needed to also wipe NVS settings. `scripts/flash.sh --erase` clears the NVS config partition (tower config, button mode) before flashing — settings will reset to defaults.

**Serial monitor garbage:** During a boot loop (no valid firmware), the ESP32 ROM bootloader outputs a binary crash dump at 115200 baud that looks like garbled characters. This is normal and disappears once the firmware is running. Set your serial monitor to 115200 baud.

**Run all API tests** (device must be powered on and workstation WiFi connected to its AP):
```bash
scripts/test.sh --api
```

**Run a single test file:**
```bash
source .venv/bin/activate
pytest tests/test_fsm_transitions.py -v
```

**Override device host:**
```bash
DMXFIRE_HOST=http://10.0.0.42 pytest tests/ -v
```

**First-time test setup:**
```bash
python3 -m venv .venv && source .venv/bin/activate && pip install -r tests/requirements.txt
```

## Conventions

- **Commits:** Always use conventional commits — `type(scope): description` (e.g. `feat(web): add brightness endpoint`, `fix(fsm): cooldown timer reset`).
- **"upload"** means compile and flash firmware to the M5AtomS3 device via `scripts/flash.sh`.
- **"push"** means push the current branch to GitHub (`git push`).
- **Scripts:** Put any regularly-run command in `scripts/` as a shell script rather than documenting a raw CLI invocation.
- **Skills:** Use Claude Code skills (`/upload`, `/test`, etc.) for repeat operations instead of re-issuing raw commands each time.
- **Specs:** Always document new features as `docs/spec-<feature>.md` — both new features being added and existing features that don't yet have a spec. Follow the format of existing specs (`spec-api-button-and-tests.md`, `spec-morse-code.md`, etc.): Context, technical details (files / endpoints / encoding), web UI changes if any, persistence, and explicit Non-goals.

## Architecture

This is an Arduino sketch (`Test_Button_DMX/`) running on an M5AtomS3 Lite (ESP32-S3). It controls DMX512 lighting on 4 towers and a propane solenoid (Confluence) over a 64-channel universe at 50 Hz, with a web config UI served from the device's WiFi AP at `192.168.4.1`.

### DMX Universe Layout

Each tower has **two fixtures** sharing one config: an accumulator **decoder** (RGB strips + fire valve on CH4) and an **uplight** (LaluceNatz LL960S in **4-channel mode** — R/G/B/W linear dimming).

```
Confluence:  CH  1– 4  (CH1 = central solenoid valve; 3-ch decoder at A001)
Tower 0:     CH  5–19  (decoder A005: strips CH5–7 + FIRE CH8; uplight A009: CH9–12; CH13–19 unclaimed)
Tower 1:     CH 20–34  (decoder A020 fire=CH23; uplight A024: CH24–27; CH28–34 unclaimed)
Tower 2:     CH 35–49  (decoder A035 fire=CH38; uplight A039: CH39–42; CH43–49 unclaimed)
Tower 3:     CH 50–64  (decoder A050 fire=CH53; uplight A054: CH54–57; CH58–64 unclaimed)
```

Valve channels: **1** (Confluence), **8 / 23 / 38 / 53** (towers). The 15-channel stride is kept even though only 8 channels per tower are claimed, so every fixture keeps its existing start address — see `docs/spec-uplight-4ch-mode.md`. All unclaimed channels are driven to 0 every frame so no stale byte can sit next to a valve channel.

**Fire and white are independent channels.** The decoder's CH4 is the fire valve (opened during `FIRE_ACTIVE` and purge); the uplight's white is CH4 of its own block, driven by themes/fire look/end-cue. The valve byte never lights white; white never opens a valve. Accumulator strip RGB is capped (75%, `STRIP_BRIGHTNESS_PCT` in `towers.cpp`) to protect the old, power-limited strips; the uplight runs full brightness.

**Strip RGB and uplight RGB are separate fields.** `TowerState` carries `r/g/b` (strips) *and* `ur/ug/ub` (uplight). `themeRender()` sets both to the same theme colour; while any valve is open the main loop overrides only the uplight with a configurable flame colour, so the strips keep animating. See `docs/spec-fire-uplight.md`.

**4-channel mode has no master dimmer and no strobe gate.** Brightness is baked into the RGB/white values by `themeRender()`. `TowerState` carries only colour plus `white`/`fire` — do not reintroduce `masterDim`/`rgbStrobe`/`wStrobe`, because in 4-channel mode those channel slots are Green and Blue.

### Key Subsystems

**FSM (`button_fsm.h/.cpp`)** — 4-state machine driven by physical GPIO39 button or API injection:
`IDLE → FIRE_ACTIVE → END_CUE → COOLDOWN → IDLE`
- Mode 0 (FIREBALL): runs full `fireDurationMs`; mode 1 (PARTY): stops on release; mode 2 (MACHINE_GUN): pulses **all five valves** on/off while held
- Cooldown lockout prevents rapid solenoid re-fire
- `endCueMs` (default 1000) sets the END_CUE length; **0 skips the state entirely**, which is what unlocks rapid retrigger. It was hardcoded at 1000 ms and was the real reason low `cooldownMs` values felt ignored.
- `fsmConsumeFirePending()` latches every entry to `FIRE_ACTIVE` so a fire window shorter than one 50 ms DMX frame still reaches the wire. **Call it exactly once per frame, before the tower loop** — it drains state. See `docs/spec-rapid-retrigger.md`.
- **The DMX bus is the floor on shot rate**, not the FSM: `DMX_FRAME_INTERVAL_MS = 50` (deliberately slow, a flicker fix) means ~100 ms is the fastest expressible shot cycle.

**Towers (`towers.h/.cpp`)** — per-tower config (theme, brightness, speed, flameLevel, connected); `towerWrite()` emits the decoder block (capped strip RGB from `r/g/b` + fire on CH4) and the uplight block (full RGB from `ur/ug/ub` + white) each tick. Idle visuals come from `themeRender()`; `flameLevel` drives the fire valve during `FIRE_ACTIVE` and purge.

**Themes (`themes.h/.cpp`)** — `themeRender(name, index, nowMs, brightness, speedPct)` returns a `TowerState` (RGB + white + fire) per tower per frame, with brightness already applied. Eight themes: gradient fire (`green`/`blue`/`fire`, 800 ms ON / 3200 ms OFF flash cycle) and procedural (`simon`, `rainbow`, `warm_white`, `bright_white`, `candle`). `speedPct` (10–400, 100 = normal) scales time. White themes drive the uplight white channel. Mirrored in `tools/web-preview/simulator.html`'s `renderTheme()` — keep in lock-step.

**Confluence (`confluence.h/.cpp`)** — central solenoid config; only CH1 matters (3-channel decoder at A001, solenoid on its first output). Fires when FSM is `FIRE_ACTIVE`, zero otherwise. Per-tower decoder CH4 valves fire in parallel. See `docs/spec-confluence-addressing.md`.

**DMX shadow buffer (`dmx.h/.cpp`)** — `dmxLastFrame[64]` mirrors every byte written; exposed via `/api/state` for test assertions.

**Web server (`web.cpp`)** — serves a tabbed mobile config UI + REST API:
- `GET /api/state` — JSON snapshot: `boot_id`, FSM state, button config (incl. `endCueMs`), `fireUplight` colour, per-tower config (`theme`/`brightness`/`speed`/`flameLevel`), full DMX frame
- `POST /set` `target=fireup` — global uplight colour held while any valve is open (`fireUpColor` as `#rrggbb`, or explicit `fireUpR/G/B/W` bytes)
- `POST /api/button/press|release|reset` — virtual button injection for tests
- `POST /api/purge/start|stop` — Empty Accumulator: hold every tower valve + Confluence solenoid open while pressed, bypassing the FSM (no `fireDurationMs` limit, no cooldown). Exposed as `purge` in `/api/state`.
- `POST /api/morse|/api/morse/stop` — Morse playback
- `POST /api/captive/dismiss` — turn off the captive-portal redirect so the OS popup closes and the operator can switch to a real browser (RAM-only flag, resets each boot). OS probe URLs (`/hotspot-detect.html`, `/generate_204`, `/ncsi.txt`, …) return success once dismissed.

**Storage (`storage.cpp`)** — NVS via ESP32 `Preferences`; loaded at `setup()`, saved on config change. Per-tower theme key is `t<N>h`; speed is `t<N>s`.

### Test Harness

Host-side Python pytest in `tests/`. `conftest.py` resets the device to a known baseline (short durations: 500 ms fire, 2000 ms cooldown) before each test. `tests/api.py` is the HTTP client wrapper. Tests require the device to be live on `192.168.4.1` (or `$DMXFIRE_HOST`).

### Logging

`log.h` provides macro-based Serial logging at 115200 baud. Compile-time level set via `LOG_LEVEL` (0–4, default 3 = INFO).
