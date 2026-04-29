# 9PCS 4IN1 LED Washer Wall Light

> **Note:** A specific online manual for this product was not found. This file contains generic LED wall washer documentation for DMX512 RGBW fixtures. Supplement or replace with the physical manual or a photo of the unit.

---

## Overview

A multi-LED RGBW wall washer fixture containing 9 RGBW (4-in-1) LED emitters. DMX512 control allows individual colour mixing, dimming, and strobe via a DMX controller.

---

## Typical specifications

| Parameter | Value |
|-----------|-------|
| LEDs | 9 × RGBW 4-in-1 emitters |
| Colour mixing | Red + Green + Blue + White |
| Control | DMX512, stand-alone, master/slave |
| DMX channels | 4 or 8 (check unit label) |
| Beam angle | Typically 20–30° per emitter |
| Connector | 3-pin or 5-pin XLR |
| Power | AC 100–240V |

---

## DMX channel mapping

### 4-channel mode (RGBW)

| CH | Function | Range |
|----|----------|-------|
| 1 | Red | 0–255 |
| 2 | Green | 0–255 |
| 3 | Blue | 0–255 |
| 4 | White | 0–255 |

### 8-channel mode (common)

| CH | Function | Range |
|----|----------|-------|
| 1 | Dimmer (master) | 0–255 |
| 2 | Red | 0–255 |
| 3 | Green | 0–255 |
| 4 | Blue | 0–255 |
| 5 | White | 0–255 |
| 6 | Strobe | 0 = off, 1–255 = slow→fast |
| 7 | Program | Built-in effects |
| 8 | Program speed | 0–255 |

> Verify channel count with the physical unit — the dipswitch or menu sets the mode.

---

## Address setting (dipswitch, common method)

DMX address is set in binary using 9 or 10 DIP switches (depending on model).

| Switch | Binary value |
|--------|-------------|
| SW1 | 1 |
| SW2 | 2 |
| SW3 | 4 |
| SW4 | 8 |
| SW5 | 16 |
| SW6 | 32 |
| SW7 | 64 |
| SW8 | 128 |
| SW9 | 256 |

Add the values of all **ON** switches to get the DMX start address.

Example: SW1 ON + SW3 ON = 1 + 4 = address **5**

---

## Daisy-chaining multiple fixtures

```
Controller ──► Fixture 1 (addr 1)  ──►  Fixture 2 (addr 5)  ──►  120Ω terminator
                  XLR Thru              XLR Thru
```

Space addresses by the number of channels each fixture uses.  
Example (4-channel mode): fixture 1 = addr 1, fixture 2 = addr 5, fixture 3 = addr 9…

---

## Wiring

### DMX (3-pin XLR)
```
Pin 1  GND / Shield
Pin 2  Data − (cold)
Pin 3  Data + (hot)
```

### DMX (5-pin XLR)
```
Pin 1  GND / Shield
Pin 2  Data − (cold)
Pin 3  Data + (hot)
Pin 4  (secondary data −, not used in standard DMX512)
Pin 5  (secondary data +, not used in standard DMX512)
```

Adapt 5-pin to 3-pin: connect pins 1→1, 2→2, 3→3; leave 4 and 5 disconnected.
