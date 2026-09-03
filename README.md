# dmx-fire

DMX512 control over flame effects and lighting. An M5AtomS3 Lite (ESP32-S3) drives a propane solenoid plus 4 towers (each an accumulator decoder + uplight) over a 64-channel universe at 50 Hz, with a tabbed mobile web UI served from the device's own WiFi AP.

## Hardware

- [M5AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) — ESP32-S3 dev board
- [Unit DMX](https://docs.m5stack.com/en/unit/Unit-DMX) — M5Stack DMX512 module

## Setup

### 1. Install dependencies

In Arduino IDE or `arduino-cli`, install:

| Library | Version |
|---------|---------|
| M5Unified | ^0.2.11 |
| FastLED | ^3.9.10 |
| SparkFun DMX Shield Library | ^2.0.1 |

Board: **M5Stack** via the [M5Stack board manager](https://m5stack.oss-cn-shenzhen.aliyuncs.com/resource/arduino/package_m5stack_index.json), select `M5AtomS3`.

### 2. Create your secrets file

Copy the example and fill in your WiFi credentials:

```bash
cp Test_Button_DMX/secrets.h.example Test_Button_DMX/secrets.h
```

Edit `secrets.h`:

```cpp
#define WIFI_SSID "your-network-name"
#define WIFI_PASS "your-password"
```

`secrets.h` is gitignored and should never be committed.

### 3. Compile and upload

```bash
arduino-cli compile --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX
scripts/flash.sh    # compiles + flashes block-by-block (see docs/spec-upload.md)
```

> The ESP32-S3 USB-Serial/JTAG controller needs a block-by-block flash; use `scripts/flash.sh`, not a raw `arduino-cli upload`. Turn Bluetooth off first and use a data-capable USB cable. See `CLAUDE.md` for details.

## Web configuration

Once running, the device broadcasts a WiFi access point using the credentials in `secrets.h`. Joining it usually pops a captive-portal page automatically; otherwise open `http://192.168.4.1`. The UI is a tabbed mobile layout:

- **Test Fire** — arm-cover-gated fire button (mirrors the physical button), plus an "Open in real browser" escape from the captive portal
- **Button Config** — mode (Fireball / Party / Machine Gun), fire duration, machine-gun burst, cooldown, end cue
- **Morse** — send a message as solenoid dots/dashes
- **Confluence** — central propane solenoid (connected + fire level)
- **Tower Configs** — per-tower (and apply-to-all) theme, brightness, speed, flame level; each sub-tab shows the DMX addresses to set

Every control auto-saves to NVS on change; settings persist across power cycles.

### Themes

Per-tower idle visuals. Gradient fire (`green`, `blue`, `fire` — flash 800 ms on / 3200 ms off) and procedural (`simon` rotating colours, `rainbow`, `warm_white`, `bright_white`, `candle`). **Speed** (10–400%) scales the animation. Colour drives the uplight RGB (full) and accumulator strips (capped); white themes drive the uplight's dedicated white channel. See `docs/spec-themes.md`.

## DMX universe (64 channels)

Confluence on ch 1–4 (3-channel decoder at A001, **solenoid on ch 1**); each tower gets a 15-channel stride holding a 4-channel decoder plus a 4-channel uplight, leaving 7 unclaimed channels that are driven to 0. **Fire (decoder CH4) and white (uplight CH4) are independent** — fire without white, white without fire. Full channel map in `docs/hardware.md`.

```
Confluence: ch 1–4     (ch 1 = central valve; ch 2–4 unclaimed)
Tower 0:    ch  5–19   decoder  5– 8 (fire= 8), uplight  9–12, 13–19 unclaimed
Tower 1:    ch 20–34   decoder 20–23 (fire=23), uplight 24–27, 28–34 unclaimed
Tower 2:    ch 35–49   decoder 35–38 (fire=38), uplight 39–42, 43–49 unclaimed
Tower 3:    ch 50–64   decoder 50–53 (fire=53), uplight 54–57, 58–64 unclaimed
```

Valve channels: **1, 8, 23, 38, 53**. Uplights run in 4-channel mode (R/G/B/W) — all four must be in the *same* mode; mixed 4-ch/8-ch caused inconsistent per-tower behaviour in the field.

## Button behaviour

| Mode | Behaviour |
|------|-----------|
| Fireball | Press → fires the full **fire duration**, ignores early release |
| Party | Fires while held, stops on release (capped at fire duration) |
| Machine Gun | Pulses the solenoid on/off every **burst** ms while held |

Cooldown locks out re-fire for the configured duration. During `FIRE_ACTIVE` each tower's decoder CH4 valve opens fully alongside the central confluence. Valve channels are binary — 0 or 255, never in between (see `docs/spec-solenoid-binary.md`).
