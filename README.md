# dmx-fire

DMX512 control over various systems, including flame effects.

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
arduino-cli upload  --fqbn m5stack:esp32:m5stack_atoms3 --port <port> Test_Button_DMX
```

## Web configuration

Once running, the device broadcasts a WiFi access point using the credentials in `secrets.h`. Connect to it and navigate to `http://192.168.4.1` to configure:

- **Idle palette** — the colour palette used when the button is not held (green fire, blue fire, or natural fire)
- **Idle brightness** — DMX output level when idle (0–255)

## DMX channels

| Channel | Signal |
|---------|--------|
| 1 | Red |
| 2 | Green |
| 3 | Blue |
| 4 | White strobe (active while button is held) |

## Button behaviour

| State | Palette | Brightness |
|-------|---------|------------|
| Idle | Web-configured | Web-configured |
| Held | Blue fire | 255 (full) |
| Released | Restores web-configured idle | — |
