# 9PCS 4IN1 LED Washer Wall Light

Source: physical manual photo (`IMG_1374.heic`)

---

## Overview

A multi-LED RGBW wall washer fixture containing 9 RGBW (4-in-1) LED emitters. Supports DMX512, stand-alone, and sound-control modes. Address and channel count are set via the front panel menu.

---

## Controls

| Button | Function |
|--------|----------|
| MENU | Enter / cycle menu |
| UP | Increment value |
| DOWN | Decrement value |
| ENTER | Confirm / save |

---

## DMX channel mapping

### 4-channel mode — set address with `d001`

| CH | Function | Range |
|----|----------|-------|
| 1 | Red | 0–255 (dark to bright) |
| 2 | Green | 0–255 (dark to bright) |
| 3 | Blue | 0–255 (dark to bright) |
| 4 | White | 0–255 (dark to bright) |

### 8-channel mode — set address with `A001`

| CH | Function | Range / Notes |
|----|----------|---------------|
| 1 | Total Dimmer | 0–255, RGBW master (dark to bright) |
| 2 | Red | 0–255 |
| 3 | Green | 0–255 |
| 4 | Blue | 0–255 |
| 5 | White | 0–255 |
| 6 | Total Strobe | 005–255 (slow to fast); 0 = off |
| 7 | Function Choice | 0–50: direct CH1–6 control; 51–100: color select (via CH8); 101–150: color jump; 151–200: color gradate; 201–250: color pulse; 251–255: sound control |
| 8 | Function Speed | Speed / color output when CH7 ≥ 51 |

> **This project uses 4-channel mode (`d001`).** In 8-channel mode (`A001`) CH1 is a master dimmer — sending 0 on CH1 blacks out the fixture regardless of other channels.

---

## Address / menu settings

| Display | Range | Description |
|---------|-------|-------------|
| `A001` | 001–512 | DMX address — **8-channel mode** |
| `d001` | 001–512 | DMX address — **4-channel mode** |
| `o255` | 000–255 | Remote control: built-in 1–9 mixed color |
| `r255` | 000–255 | Manual R dimming |
| `G255` | 000–255 | Manual G dimming |
| `b255` | 000–255 | Manual B dimming |
| `u255` | 010–255 | Manual W dimming |
| `FH99` | 00–99 | Strobe speed (slow to fast) |
| `CL01` | 01–08 | 8 color presets |
| `CC99` | 00–99 | Color jump speed |
| `DE99` | 00–99 | Color gradate speed |
| `CP99` | 00–99 | Color pulse speed |
| `soUd` | — | Sound control mode |
| `rFye` | ye/no | Infrared remote control on/off |

---

## Wiring (XLR-3)

| Pin | Signal |
|-----|--------|
| 1 | GND / Shield |
| 2 | Data − (cold) |
| 3 | Data + (hot) |
