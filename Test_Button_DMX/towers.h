#pragma once
#include <Arduino.h>
#include <FastLED.h>

static const uint8_t NUM_TOWERS = 4;

// State sent to every fixture in a tower each DMX frame.
struct TowerState {
  uint8_t r, g, b;      // colour — written to decoder CH1-3 and strobe CH5-7
  uint8_t masterDim;    // strobe CH1: overall brightness (0–255)
  uint8_t rgbStrobe;    // strobe CH2: RGB strobe speed (0=off, 1–255=slow→fast)
  uint8_t wStrobe;      // strobe CH8: white strobe speed (0=off, 1–255=slow→fast)
  uint8_t wDim;         // strobe CH11 + decoder CH4: white level (0–255)
};

// Web-configurable idle state for one tower.
struct TowerConfig {
  bool            connected;   // physically present (tracked in web UI)
  String          palName;     // "green", "blue", "fire" — for web UI rendering
  CRGBPalette256  pal;         // active idle palette
  uint8_t         bright;      // idle brightness 0–255
  uint8_t         flameLevel;  // 0=off, 255=full open; written to decoder CH4 (W) during fire
};

extern TowerConfig towerConfigs[NUM_TOWERS];
extern bool        towerConfigUpdated;  // set by web handler when config changes

// Initialise towerConfigs to defaults. Call from setup() before webSetup().
void towerSetup();

// Write state to a single tower (index 0–3).
void towerWrite(uint8_t index, const TowerState& state);

// Write the same state to all towers, then flush DMX.
void towersWrite(const TowerState& state);
