#pragma once
#include <Arduino.h>

static const uint8_t NUM_TOWERS = 4;

// State sent to every fixture in a tower each DMX frame.
struct TowerState {
  uint8_t r, g, b;      // colour — written to decoder CH1-3 and strobe CH5-7
  uint8_t masterDim;    // strobe CH1: overall brightness (0–255)
  uint8_t rgbStrobe;    // strobe CH2: RGB strobe speed (0=off, 1–255=slow→fast)
  uint8_t wStrobe;      // strobe CH8: white strobe speed (0=off, 1–255=slow→fast)
  uint8_t wDim;         // strobe CH11 + decoder CH4: white level (0–255)
};

// Write state to a single tower (index 0–3).
void towerWrite(uint8_t index, const TowerState& state);

// Write the same state to all towers, then flush DMX.
void towersWrite(const TowerState& state);
