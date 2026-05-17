#include "palettes.h"

// https://github.com/FastLED/FastLED/wiki/Gradient-color-palettes
// Remember: the palette index wraps 0-255, so align your 0 and 255 entries
// to avoid a jerky color transition at the wrap point.

DEFINE_GRADIENT_PALETTE(firepal){
  30,  255, 255,   0,   // yellow (middle of flames)
  65,  255,   0,   0,   // red    (base of flames)
  225, 255, 255,   0,   // yellow (middle of flames)
  255, 255, 255, 255    // white  (hottest tips)
};

DEFINE_GRADIENT_PALETTE(electricGreenFirePal){
  0,     0,  32,   0,   // very dark green
  32,    0,  70,   0,   // dark green (base)
  190,  57, 255,  20,   // electric neon green (middle)
  255, 255, 255, 255    // white (hottest tips)
};

DEFINE_GRADIENT_PALETTE(electricBlueFirePal){
  0,     0,   0,   0,   // black (base)
  32,    0,   0,  70,   // dark blue
  128,  20,  57, 255,   // electric blue (middle)
  255, 255, 255, 255    // white (hottest tips)
};

CRGBPalette256 palFromName(const String& name) {
  if (name == "blue") return electricBlueFirePal;
  if (name == "fire") return firepal;
  return electricGreenFirePal;  // default: "green"
}
