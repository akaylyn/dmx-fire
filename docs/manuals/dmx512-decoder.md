# DMX512 Decoder with Digital Display

> **Note:** This file covers generic DMX512 decoder operation. The specific model in use was not identified online — supplement or replace with the physical manual or a photo of the unit.

---

## Overview

A DMX512 decoder receives a DMX512 data stream and converts selected channels to PWM or constant-current output for driving LED loads (single colour, RGB, or RGBW strips/fixtures).

The digital display shows the active DMX start address and allows address setting via front-panel buttons.

---

## Typical specifications

| Parameter | Common values |
|-----------|--------------|
| Input voltage | DC 5–24V (check label) |
| Output channels | 3 (RGB) or 4 (RGBW) |
| Output current per channel | 4A or 8A typical |
| Max load | Depends on supply voltage and channel count |
| DMX channels used | 3 (RGB) or 4 (RGBW) |
| Protocol | DMX512 (250 kbps, 8N2) |
| Address range | 1–510 (RGB) / 1–509 (RGBW) |

---

## Address setting

1. Press **SET** (or **+**/**−**) to enter address mode — display flashes
2. Use **+**/**−** to increment/decrement the address
3. Press **SET** again (or wait) to confirm
4. The decoder listens to the configured address and the next N−1 channels (N = number of output channels)

Example: address set to **001**, RGB mode → listens to channels 1 (R), 2 (G), 3 (B).

---

## Wiring

### DMX input
```
DMX source  pin 1 (GND/Shield) ──► Decoder GND (optional, for shielding)
            pin 2 (Data −)     ──► Decoder DMX−
            pin 3 (Data +)     ──► Decoder DMX+
```

Most decoders also accept a bare 2-wire connection (Data+ / Data−) without shield.

### LED output
```
Decoder V+  ──►  LED strip V+
Decoder CH1 ──►  LED strip R (or single channel)
Decoder CH2 ──►  LED strip G
Decoder CH3 ──►  LED strip B
Decoder CH4 ──►  LED strip W  (RGBW decoders only)
Decoder GND ──►  LED strip GND
```

### Power
```
Power supply (+) ──► Decoder V+
Power supply (−) ──► Decoder GND
```

---

## DMX channel mapping (this project)

This project uses decoders in **two different roles**, wired and addressed differently. Getting
them mixed up means either no fire or uncommanded fire, so check the role before setting an address.

### Role 1 — Confluence solenoid driver (×1)

A **3-channel** decoder at start address **A001**, listening on universe ch 1–3. The propane
solenoid is wired to the decoder's **first output**; outputs 2 and 3 are unwired.

| Decoder output | Universe CH | Signal |
|----------------|-------------|--------|
| 1 | **1** | **Propane solenoid** — 255 while firing, 0 otherwise (binary; see spec-solenoid-binary.md) |
| 2 | 2 | unwired (written 0) |
| 3 | 3 | unwired (written 0) |

Universe ch 4 is claimed by no fixture and is written 0 every frame.

### Role 2 — Accumulator decoders (×4, one per tower)

**4-channel** decoders at start addresses **A005 / A020 / A035 / A050** (tower 0–3). Outputs 1–3
drive the RGB LED strips wrapped around the accumulator tank; output 4 drives that tower's
propane valve.

| Decoder output | Signal | Notes |
|----------------|--------|-------|
| 1 | Strip Red | Capped to 75% (`STRIP_BRIGHTNESS_PCT`) — old, power-limited strips |
| 2 | Strip Green | Capped |
| 3 | Strip Blue | Capped |
| 4 | **Propane valve** | 255 while firing/purging, 0 otherwise. Binary only. **Never carries white.** |

Resolved valve channels: tower 0 = ch 8, tower 1 = ch 23, tower 2 = ch 38, tower 3 = ch 53.

> The tower **uplights** are not decoders — they are LaluceNatz LL960S fixtures in 4-channel mode
> at A009 / A024 / A039 / A054. See [../hardware.md](../hardware.md) for the full universe map.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| No output | Wrong DMX address, missing termination, or signal polarity swapped |
| Flickering | Missing 120Ω terminator at end of chain |
| Wrong colours | DMX− and DMX+ swapped, or wrong channel mode on decoder |
| Display shows `- - -` | No DMX signal received |
