# Hardware Reference

All hardware used in this project, with links to offline documentation and original sources.

---

## Controller

| Component | Description | Offline docs |
|-----------|-------------|-------------|
| [M5AtomS3 Lite](https://docs.m5stack.com/en/core/AtomS3%20Lite) | ESP32-S3 dev board, 8MB flash, WiFi, BLE, WS2812 LED, 1 button | [manuals/m5atomS3-lite.md](manuals/m5atomS3-lite.md) |
| [M5Stack Unit DMX](https://docs.m5stack.com/en/unit/Unit-DMX) | Isolated RS-485 / DMX512 transceiver with XLR-3 connector | [manuals/m5stack-unit-dmx.md](manuals/m5stack-unit-dmx.md) |

---

## Fixtures

| Component | Description | Offline docs |
|-----------|-------------|-------------|
| [LaluceNatz LL960S Strobe](https://www.lalucenatzonline.com/products/500w-960led-rgb-w-dj-strobe-light-panel-with-zone-control-for-concert-disco-party-light-free-shipping) | 960-LED RGBW 4-in-1 stage strobe, 240W, 8 segments, 4/11/32/39-ch DMX | [manuals/strobe-lalucenatz-500w-rgbw.md](manuals/strobe-lalucenatz-500w-rgbw.md) |
| DMX512 Decoder (digital display) | DMX512-to-PWM decoder for LED strips | [manuals/dmx512-decoder.md](manuals/dmx512-decoder.md) |
| 9PCS 4IN1 LED Washer Wall Light | RGBW wall wash fixture, 9× 4-in-1 emitters | [manuals/led-washer-wall-light-9pcs-4in1.md](manuals/led-washer-wall-light-9pcs-4in1.md) |

---

## Libraries

| Library | Version | Purpose | Offline docs |
|---------|---------|---------|-------------|
| [M5Unified](https://github.com/m5stack/M5Unified) | 0.2.4 | Board init, buttons | [libraries/M5Unified.md](libraries/M5Unified.md) |
| [FastLED](https://github.com/FastLED/FastLED) | 3.9.13 | Colour palettes, onboard LED | [libraries/FastLED.md](libraries/FastLED.md) |
| [SparkFun DMX Shield Library](https://github.com/sparkfun/SparkFunDMX) | 2.0.1 | DMX512 output | [libraries/SparkFunDMX.md](libraries/SparkFunDMX.md) |
| WebServer (ESP32 core) | 3.3.7 | HTTP config server | [libraries/WebServer.md](libraries/WebServer.md) |

---

## DMX channel assignment (this project)

The central propane solenoid (Confluence) occupies channels 1–4. Each of the 4 towers then occupies a contiguous 15-channel block: a 4-channel **accumulator decoder** (RGB strips + fire valve) followed by an 11-channel **uplight** (LaluceNatz LL960S in 11-ch mode).

```
Confluence: ch  1–4    (ch 4 = central solenoid valve)
Tower 0:    ch  5–19
Tower 1:    ch 20–34
Tower 2:    ch 35–49
Tower 3:    ch 50–64
```

**Per tower** (base = 4 + towerIndex × 15):

| Offset | Fixture CH | Device | Signal | Notes |
|--------|-----------|--------|--------|-------|
| +1 | 1 | Decoder | Red (strip) | Capped to `STRIP_BRIGHTNESS_PCT` (75%) — old/power-limited strips |
| +2 | 2 | Decoder | Green (strip) | Capped |
| +3 | 3 | Decoder | Blue (strip) | Capped |
| +4 | 4 | Decoder | **FIRE valve** | Propane only. Driven by `flameLevel` during `FIRE_ACTIVE`, 0 otherwise. **Never carries white.** |
| +5 | 1 | Uplight | Master dim | Always 255 (full) |
| +6 | 2 | Uplight | RGB strobe speed | 1 = open/steady (≠0, or LL960S blanks RGB) |
| +7 | 3 | Uplight | RGB mode | Hardcoded 0 (direct colour) |
| +8 | 4 | Uplight | RGB mode speed | Hardcoded 0 |
| +9 | 5 | Uplight | Red (full) | Theme colour, full brightness |
| +10 | 6 | Uplight | Green (full) | |
| +11 | 7 | Uplight | Blue (full) | |
| +12 | 8 | Uplight | White strobe speed | 0 (no strobe) |
| +13 | 9 | Uplight | White mode | Hardcoded 0 (direct) |
| +14 | 10 | Uplight | White mode speed | Hardcoded 0 |
| +15 | 11 | Uplight | **White dimmer** | White themes / end-cue flash. **Independent of fire.** |

Set each accumulator decoder to its tower's start address (**A005, A020, A035, A050**).  
Set each uplight to its tower's start address + 4 (**A009, A024, A039, A054**), 11-channel mode.

The web UI prints these addresses per tower (in each Tower Configs sub-tab) so you can dial them in while looking at the page.

### Fire vs. white independence

The decoder's CH4 (fire valve) and the uplight's white channel are physically separate wires. This is deliberate: you can fire without lighting the towers white (fire during any colour theme), and you can send white without opening a valve (select `bright_white`/`warm_white`/`candle`, or the end-cue flash). The accumulator strips are RGB-only — to get "white" there you'd send full R+G+B, which overdraws the old supply, so the white themes drive the uplight's dedicated W channel instead and leave the strips dark.

---

## Incomplete manuals

The following docs were not available online and contain generic content only. If you have the physical manuals, photos can be used to fill in the missing sections (particularly DMX channel tables):

- [manuals/dmx512-decoder.md](manuals/dmx512-decoder.md) — model not identified; content is generic
- [manuals/led-washer-wall-light-9pcs-4in1.md](manuals/led-washer-wall-light-9pcs-4in1.md) — content is generic

---

## Wiring overview

```
[M5AtomS3 Lite]
  PORTA G1 (RX) ──────────────────────────────────┐
  PORTA G2 (TX) ──────────────────────────────────┤
                                          [Unit DMX]
                                          XLR-3 out ──► [Strobe light]
                                                              XLR Thru ──► [Decoder]
                                                                               XLR Thru ──► [Wall Washer]
                                                                                                 └── 120Ω terminator
  GPIO 39 ◄── [External button] (pull to GND)
  GPIO 35 ──► [Onboard WS2812 LED] (built-in)
```
