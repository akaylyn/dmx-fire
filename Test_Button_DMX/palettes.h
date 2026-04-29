#pragma once
#include <Arduino.h>
#include <FastLED.h>

// Palette data — defined in palettes.cpp
DECLARE_GRADIENT_PALETTE(firepal);
DECLARE_GRADIENT_PALETTE(electricGreenFirePal);
DECLARE_GRADIENT_PALETTE(electricBlueFirePal);

// Map a palette name ("green", "blue", "fire") to a CRGBPalette256.
// Returns electricGreenFirePal for unrecognised names.
CRGBPalette256 palFromName(const String& name);
