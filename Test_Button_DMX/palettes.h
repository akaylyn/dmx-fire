#pragma once
#include <FastLED.h>

// Palette data — defined in palettes.cpp
DECLARE_GRADIENT_PALETTE(firepal);
DECLARE_GRADIENT_PALETTE(electricGreenFirePal);
DECLARE_GRADIENT_PALETTE(electricBlueFirePal);

// Web-configured idle state
extern CRGBPalette256 idlePal;
extern byte           idleBright;
extern String         idlePalName;

// Active state — may be temporarily overridden by the button
extern CRGBPalette256 currPal;
extern byte           currBright;

// Set by the web handler; consumed by the main loop to apply idle changes
extern bool idleUpdated;
