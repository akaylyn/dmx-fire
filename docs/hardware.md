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
| 9PCS 4IN1 LED Washer Wall Light | **Bench test light only — not part of the installed rig.** RGBW wall wash fixture, 9× 4-in-1 emitters | [manuals/led-washer-wall-light-9pcs-4in1.md](manuals/led-washer-wall-light-9pcs-4in1.md) |

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

The central propane solenoid (Confluence) sits at the bottom of the universe on a 3-channel decoder. Each of the 4 towers then gets a 15-channel stride: a 4-channel **accumulator decoder** (RGB strips + fire valve) followed by a 4-channel **uplight** (LaluceNatz LL960S in 4-ch mode), leaving 7 unclaimed channels per tower.

```
Confluence: ch  1–4    (ch 1 = central solenoid valve; ch 2–4 unclaimed)
Tower 0:    ch  5–19   (decoder 5–8, uplight 9–12, 13–19 unclaimed)
Tower 1:    ch 20–34   (decoder 20–23, uplight 24–27, 28–34 unclaimed)
Tower 2:    ch 35–49   (decoder 35–38, uplight 39–42, 43–49 unclaimed)
Tower 3:    ch 50–64   (decoder 50–53, uplight 54–57, 58–64 unclaimed)
```

**Valve channels: 1, 8, 23, 38, 53.** Nothing else opens propane.

**Per tower** (base = 4 + towerIndex × 15):

| Offset | Fixture CH | Device | Signal | Notes |
|--------|-----------|--------|--------|-------|
| +1 | 1 | Decoder | Red (strip) | Capped to `STRIP_BRIGHTNESS_PCT` (75%) — old/power-limited strips |
| +2 | 2 | Decoder | Green (strip) | Capped |
| +3 | 3 | Decoder | Blue (strip) | Capped |
| +4 | 4 | Decoder | **FIRE valve** | Propane only. Driven by `flameLevel` during `FIRE_ACTIVE`/purge, 0 otherwise. **Never carries white.** |
| +5 | 1 | Uplight | Red | Theme colour, full brightness (uncapped) |
| +6 | 2 | Uplight | Green | |
| +7 | 3 | Uplight | Blue | |
| +8 | 4 | Uplight | **White** | White themes / end-cue flash. **Independent of fire.** |
| +9 … +15 | — | — | *unclaimed* | No fixture listens; driven to 0 every frame |

4-channel mode is plain linear dimming per colour — there is no master dimmer and no strobe gate, so brightness is baked into the values by `themeRender()`.

Set each accumulator decoder to its tower's start address (**A005, A020, A035, A050**).  
Set each uplight to its tower's start address + 4 (**A009, A024, A039, A054**), **4-channel mode**.  
Set the Confluence decoder to **A001**, 3-channel mode, solenoid on its first output.

> **All four uplights must be in the same 4-channel mode.** Mixed modes cause inconsistent per-tower behaviour: the fixture still responds to the bus, just never as intended, which reads as a wiring or cabling fault.
>
> **On the LL960S, address and channel mode are set independently.** Dialling the address to 9 does *not* select 4-channel mode — the mode is a separate menu item (`CH04` / `CH11` / `CH32` / `CH39`) and the factory default is **`CH11`** ([manual](manuals/strobe-lalucenatz-500w-rgbw.md)). Each uplight needs **both** its address (9 / 24 / 39 / 54) **and** `CH04` set. An uplight left on `CH11` reads the firmware's Red as a master dimmer, its Green as strobe speed, and its **Blue as the built-in-effect selector** — so it runs one of the fixture's 84 internal chase patterns instead of showing theme colour.

The web UI prints these addresses per tower (in each Tower Configs sub-tab) so you can dial them in while looking at the page.

### Fire vs. white independence

The decoder's CH4 (fire valve) and the uplight's CH4 (white) are physically separate wires on separate fixtures. This is deliberate: you can fire without lighting the towers white (fire during any colour theme), and you can send white without opening a valve (select `bright_white`/`warm_white`/`candle`, or the end-cue flash). The accumulator strips are RGB-only — to get "white" there you'd send full R+G+B, which overdraws the old supply, so the white themes drive the uplight's dedicated W channel instead and leave the strips dark.

---

## Incomplete manuals

The following docs were not available online and contain generic content only. If you have the physical manuals, photos can be used to fill in the missing sections (particularly DMX channel tables):

- [manuals/dmx512-decoder.md](manuals/dmx512-decoder.md) — model not identified; content is generic
- [manuals/led-washer-wall-light-9pcs-4in1.md](manuals/led-washer-wall-light-9pcs-4in1.md) — content is generic

---

## Wiring overview

**Bench setup** (what's on the desk, including the test-only wall washer):

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

**Installed rig** — one daisy chain, 120 Ω terminator at the **last fixture only**:

```
[M5AtomS3 Lite] ──► [Unit DMX] XLR-3 out
   └─► [Confluence decoder A001]  (solenoid, ch 1)
        └─► [T0 decoder A005] ─► [T0 uplight A009 + CH04]
             └─► [T1 decoder A020] ─► [T1 uplight A024 + CH04]
                  └─► [T2 decoder A035] ─► [T2 uplight A039 + CH04]
                       └─► [T3 decoder A050] ─► [T3 uplight A054 + CH04]
                            └── 120Ω terminator
```

Nine fixtures is a lot of unit loads for the small RS-485 driver on the shield; signal degrades as towers are added. See [../notes.md](../notes.md) for the field investigation into line noise and spurious firing.
