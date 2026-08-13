#pragma once
#include <Arduino.h>
#include <FastLED.h>

static const uint8_t NUM_TOWERS = 4;

// State sent to every fixture in a tower each DMX frame.
//
// The uplight runs in 4-channel mode (R/G/B/W linear dimming), so there is no
// master dimmer and no strobe channel to drive: brightness is baked into
// r/g/b/ur/ug/ub/white by themeRender().
//
// Strip RGB and uplight RGB are SEPARATE fields so the uplight can show the
// fire look while the accumulator strips keep running the theme underneath.
// themeRender() sets both to the same colour; the main loop overrides only
// ur/ug/ub (and white) while a valve is open — see docs/spec-fire-uplight.md.
struct TowerState {
  uint8_t r, g, b;      // theme colour — accumulator strips only (capped in towerWrite)
  uint8_t ur, ug, ub;   // uplight RGB (full) — theme colour, or the fire look while firing
  uint8_t white;        // uplight white channel (4-ch mode CH4) — independent of fire
  uint8_t fire;         // accumulator decoder CH4 — propane valve, FIRE_ACTIVE/purge only
};

// Web-configurable idle state for one tower.
struct TowerConfig {
  bool      connected;   // physically present (tracked in web UI)
  String    themeName;   // "green","blue","fire","simon","rainbow","warm_white","bright_white","candle"
  uint8_t   bright;      // idle brightness 0–255
  uint16_t  speed;       // theme speed % (10..400, 100 = normal)
  uint8_t   flameLevel;  // 0=off, 255=full open; written to decoder CH4 during fire
};

extern TowerConfig towerConfigs[NUM_TOWERS];
extern bool        towerConfigUpdated;  // set by web handler when config changes

// Initialise towerConfigs to defaults. Call from setup() before webSetup().
void towerSetup();

// Write state to a single tower (index 0–3).
void towerWrite(uint8_t index, const TowerState& state);

// Write the same state to all towers, then flush DMX.
void towersWrite(const TowerState& state);
