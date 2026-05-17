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

The DMX output from the M5AtomS3 controller is:

| DMX CH | Signal |
|--------|--------|
| 1 | Red |
| 2 | Green |
| 3 | Blue |
| 4 | White strobe |

Set the decoder's start address to **1** to align with the controller output.

---

## Troubleshooting

| Symptom | Likely cause |
|---------|-------------|
| No output | Wrong DMX address, missing termination, or signal polarity swapped |
| Flickering | Missing 120Ω terminator at end of chain |
| Wrong colours | DMX− and DMX+ swapped, or wrong channel mode on decoder |
| Display shows `- - -` | No DMX signal received |
