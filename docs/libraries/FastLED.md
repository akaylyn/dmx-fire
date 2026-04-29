# FastLED

**Version:** 3.9.13  
**Source:** https://github.com/FastLED/FastLED  
**Docs:** https://fastled.io/docs  
**License:** MIT

Universal LED library supporting 30,000+ LEDs on ESP32, Arduino, Teensy, and 50+ other platforms. Used in this project for the M5AtomS3 Lite's onboard WS2812 RGB LED and for colour palette generation fed to the DMX output.

---

## Installation

Arduino Library Manager: search **FastLED**

---

## Key types

| Type | Description |
|------|-------------|
| `CRGB` | RGB colour — three `uint8_t` fields: `.r`, `.g`, `.b` |
| `CHSV` | HSV colour — hue (0–255), saturation, value |
| `CRGBPalette16` | 16-entry colour lookup table |
| `CRGBPalette256` | 256-entry colour lookup table (higher resolution) |

---

## Initialisation

```cpp
#include <FastLED.h>

#define NUM_LEDS 1
#define DATA_PIN 35
CRGB leds[NUM_LEDS];

FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
FastLED.setBrightness(255);  // 0–255 global brightness scale
```

### Chipset identifiers (common)

| Chipset | Identifier |
|---------|-----------|
| WS2812 / WS2812B (Neopixel) | `WS2812` |
| APA102 | `APA102` |
| SK6812 | `SK6812` |

### Colour order

The M5AtomS3 Lite WS2812C uses **GRB** order (not RGB).

---

## Colour

```cpp
leds[0] = CRGB::Red;           // named colour
leds[0] = CRGB(255, 0, 0);     // explicit RGB
leds[0] = CHSV(0, 255, 255);   // HSV — red, full saturation, full brightness
leds[0] = CRGB::Black;         // off

FastLED.show();                 // push to hardware — call after every change
```

### HSV hue reference (0–255)

| Value | Colour |
|-------|--------|
| 0 / 255 | Red |
| 32 | Orange |
| 64 | Yellow |
| 96 | Green |
| 128 | Aqua |
| 160 | Blue |
| 192 | Purple |
| 224 | Pink |

---

## Colour palettes

Palettes map a 0–255 index to an RGB colour, enabling smooth animations by stepping the index each frame.

### Defining a gradient palette

```cpp
DEFINE_GRADIENT_PALETTE(myPal) {
  //  index   R    G    B
      0,      0,   0,   0,   // black at index 0
      128,  255,   0,   0,   // red at index 128
      255,  255, 255, 255    // white at index 255
};
```

> The index wraps 0→255 in a cycle. If the colour at index 0 and 255 differ, the transition will be abrupt. Align them to avoid a visual jump.

### Declaring a palette in a header

```cpp
DECLARE_GRADIENT_PALETTE(myPal);  // in .h file
```

### Loading and using a palette

```cpp
CRGBPalette256 pal = myPal;       // expands gradient into 256 entries

static uint8_t idx = 0;
CRGB c = ColorFromPalette(pal, idx++, brightness, LINEARBLEND);
```

### `ColorFromPalette()` parameters

| Parameter | Description |
|-----------|-------------|
| `palette` | `CRGBPalette16` or `CRGBPalette256` |
| `index` | 0–255 position in the palette |
| `brightness` | 0–255 output brightness scale |
| `blendType` | `LINEARBLEND` (smooth) or `NOBLEND` (stepped) |

---

## Timing helpers

```cpp
EVERY_N_MILLISECONDS(20) {
    // runs at ~50Hz
}

EVERY_N_MILLISECONDS(1000) {
    // runs once per second
}
```

These use internal static timers; multiple `EVERY_N_MILLISECONDS` blocks with the same interval are independent.

---

## Common named colours

`CRGB::Red`, `CRGB::Green`, `CRGB::Blue`, `CRGB::White`, `CRGB::Black`, `CRGB::Yellow`, `CRGB::Cyan`, `CRGB::Magenta`, `CRGB::Orange`, `CRGB::Purple`
