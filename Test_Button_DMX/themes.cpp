#include <FastLED.h>
#include "themes.h"

// Gradient palettes for the fire-themed entries. Mirror of tools/web-preview/
// simulator.html's PALETTES table — keep in sync.
DEFINE_GRADIENT_PALETTE(firepal){
  30,  255, 255,   0,
  65,  255,   0,   0,
  225, 255, 255,   0,
  255, 255, 255, 255
};
DEFINE_GRADIENT_PALETTE(electricGreenFirePal){
  0,     0,  32,   0,
  32,    0,  70,   0,
  190,  57, 255,  20,
  255, 255, 255, 255
};
DEFINE_GRADIENT_PALETTE(electricBlueFirePal){
  0,     0,   0,   0,
  32,    0,   0,  70,
  128,  20,  57, 255,
  255, 255, 255, 255
};

static CRGBPalette256 palForName(const String& name) {
  if (name == "blue") return electricBlueFirePal;
  if (name == "fire") return firepal;
  return electricGreenFirePal;  // default: "green"
}

// Simon: 4 colours, rotating assignment. tower i at beat b -> SIMON[(i - b + 4) % 4].
struct SimonColor { uint8_t r, g, b; };
static const SimonColor SIMON[4] = {
  { 255,   0,   0 },  // red
  {   0,   0, 255 },  // blue
  { 255, 220,   0 },  // yellow
  {   0, 200,   0 }   // green
};

// Convert HSV (h in [0..1], s/v in [0..1]) to RGB bytes. Cheap, branchy.
static void hsvToRgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  int   i = (int)floorf(h * 6.0f);
  float f = h * 6.0f - i;
  float p = v * (1.0f - s);
  float q = v * (1.0f - f * s);
  float t = v * (1.0f - (1.0f - f) * s);
  float rf = 0, gf = 0, bf = 0;
  switch (((i % 6) + 6) % 6) {
    case 0: rf = v; gf = t; bf = p; break;
    case 1: rf = q; gf = v; bf = p; break;
    case 2: rf = p; gf = v; bf = t; break;
    case 3: rf = p; gf = q; bf = v; break;
    case 4: rf = t; gf = p; bf = v; break;
    case 5: rf = v; gf = p; bf = q; break;
  }
  r = (uint8_t)roundf(rf * 255.0f);
  g = (uint8_t)roundf(gf * 255.0f);
  b = (uint8_t)roundf(bf * 255.0f);
}

// Cheap candle flicker — two summed sines at incommensurate periods.
// Returns 0..1. Mirrors simulator.html's flicker().
static float flicker01(uint8_t seed, float t) {
  float a = sinf(t * 0.013f + seed * 7.3f);
  float b = sinf(t * 0.027f + seed * 2.1f);
  float c = sinf(t * 0.059f + seed * 11.7f);
  float v = (a + b * 0.5f + c * 0.25f) / 1.75f;
  return (v + 1.0f) * 0.5f;
}

static uint32_t scaleTime(uint32_t nowMs, uint16_t speedPct) {
  if (speedPct == 100) return nowMs;
  return (uint32_t)((uint64_t)nowMs * speedPct / 100);
}

// Theme body. Sets only the strip-side r/g/b plus white; the public wrapper
// below mirrors r/g/b into the uplight fields so no theme has to know about the
// strip/uplight split. Declared ahead of use for the unknown-theme recursion.
static TowerState themeRenderInner(const String& name, uint8_t index, uint32_t nowMs,
                                   uint8_t brightness, uint16_t speedPct);

TowerState themeRender(const String& name, uint8_t index, uint32_t nowMs,
                       uint8_t brightness, uint16_t speedPct) {
  TowerState s = themeRenderInner(name, index, nowMs, brightness, speedPct);
  // By default the uplight shows exactly what the strips show. The main loop
  // overrides ur/ug/ub while a valve is open — see docs/spec-fire-uplight.md.
  s.ur = s.r;
  s.ug = s.g;
  s.ub = s.b;
  return s;
}

static TowerState themeRenderInner(const String& name, uint8_t index, uint32_t nowMs,
                                   uint8_t brightness, uint16_t speedPct) {
  // The uplight runs in 4-channel mode (R/G/B/W linear dimming), so brightness
  // is baked straight into r/g/b/white below — there is no master dimmer to set
  // and no strobe gate to hold open.
  TowerState s = {};

  uint32_t t = scaleTime(nowMs, speedPct);

  if (name == "green" || name == "blue" || name == "fire") {
    // 4 s cycle, 800 ms ON, rest OFF.
    uint32_t phase = t % 4000;
    if (phase >= 800) return s;  // OFF — zeroed state
    uint8_t idx = (uint8_t)((t / 20) & 0xff);
    CRGB c = ColorFromPalette(palForName(name), idx, brightness, LINEARBLEND);
    s.r     = c.r;
    s.g     = c.g;
    s.b     = c.b;
    s.white = 0;  // colour themes leave the uplight white channel off
    return s;
  }

  if (name == "simon") {
    uint32_t beat = t / 1000;
    const SimonColor& c = SIMON[((index - beat) % 4 + 4) % 4];
    float scale = brightness / 255.0f;
    s.r = (uint8_t)roundf(c.r * scale);
    s.g = (uint8_t)roundf(c.g * scale);
    s.b = (uint8_t)roundf(c.b * scale);
    s.white = 0;
    return s;
  }

  if (name == "rainbow") {
    float hue = fmodf(((float)t / 8000.0f) + index * 0.25f, 1.0f);
    if (hue < 0) hue += 1.0f;
    uint8_t r, g, b;
    hsvToRgb(hue, 1.0f, 1.0f, r, g, b);
    float scale = brightness / 255.0f;
    s.r = (uint8_t)roundf(r * scale);
    s.g = (uint8_t)roundf(g * scale);
    s.b = (uint8_t)roundf(b * scale);
    s.white = 0;
    return s;
  }

  // White themes drive RGB (so the colour shows on the capped accumulator
  // strips) PLUS the uplight's dedicated white channel: warm_white/candle use a
  // warm RGB mix, bright_white uses full white. None touch the fire valve
  // (decoder CH4) — that channel is mapped separately and only opens during
  // FIRE_ACTIVE. Strip power stays safe via STRIP_BRIGHTNESS_PCT in towerWrite.
  if (name == "warm_white") {
    uint8_t dim = (uint8_t)roundf(brightness * 0.45f);
    s.r     = dim;
    s.g     = (uint8_t)roundf(dim * 0.55f);
    s.b     = (uint8_t)roundf(dim * 0.18f);
    s.white = dim;
    return s;
  }

  if (name == "bright_white") {
    s.r = s.g = s.b = brightness;  // full white on RGB (strips capped in towerWrite)
    s.white = brightness;
    return s;
  }

  if (name == "candle") {
    float f = flicker01(index, (float)t);
    float lvl = brightness * (0.55f + f * 0.45f);
    s.r     = (uint8_t)roundf(lvl);
    s.g     = (uint8_t)roundf(lvl * 0.55f);
    s.b     = (uint8_t)roundf(lvl * 0.18f);
    s.white = (uint8_t)roundf(lvl);
    return s;
  }

  // Unknown theme — fall back to green.
  return themeRender(F("green"), index, nowMs, brightness, speedPct);
}
