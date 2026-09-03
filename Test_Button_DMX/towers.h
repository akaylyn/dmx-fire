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
  // The propane valve on the accumulator decoder's CH4. A bool, not a byte:
  // a solenoid is open or shut, and there is no third thing it can be. Written
  // through dmxValveWrite(), which emits 0 or 255 — see docs/spec-solenoid-binary.md.
  bool    fireOpen;     // FIRE_ACTIVE / purge only
};

// Web-configurable idle state for one tower.
struct TowerConfig {
  bool      connected;   // physically present (tracked in web UI)
  String    themeName;   // "green","blue","fire","simon","rainbow","warm_white","bright_white","candle"
  uint8_t   bright;      // idle brightness 0–255
  uint16_t  speed;       // theme speed % (10..400, 100 = normal)
  // Whether this tower's propane valve may open at all. Replaces the old
  // flameLevel byte, which looked like a flame-size dial and was not one: the
  // solenoid is on/off, so every value below the decoder's turn-on threshold
  // just left the valve shut or chattered the coil. Flame size is gas pressure
  // and orifice, not DMX.
  //
  // Kept as a boolean because `connected` is too blunt for it: unticking that
  // blanks the tower's lights as well, so isolating one leaking tower used to
  // mean going dark. See docs/spec-solenoid-binary.md.
  bool      fireEnabled;  // false = this valve never opens; lights keep running
};

extern TowerConfig towerConfigs[NUM_TOWERS];
extern bool        towerConfigUpdated;  // set by web handler when config changes

// Initialise towerConfigs to defaults. Call from setup() before webSetup().
void towerSetup();

// Write state to a single tower (index 0–3).
void towerWrite(uint8_t index, const TowerState& state);

// The DMX channel of this tower's propane valve (the decoder's CH4). Exposed so
// callers can close a valve WITHOUT composing a whole TowerState — the main loop
// needs exactly that for a tower marked disconnected, whose block it otherwise
// skips entirely. The stride math stays private to towers.cpp.
// Always one of dmx.h's VALVE_CHANNELS; tests.cpp checks that every frame boot.
uint16_t towerValveChannel(uint8_t index);

// Write the same state to all towers, then flush DMX.
void towersWrite(const TowerState& state);
