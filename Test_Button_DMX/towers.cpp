#include <Arduino.h>
#include "dmx.h"
#include "towers.h"

TowerConfig towerConfigs[NUM_TOWERS];
bool        towerConfigUpdated = false;

void towerSetup() {
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    towerConfigs[i].connected   = true;
    towerConfigs[i].themeName   = "green";
    towerConfigs[i].bright      = 128;
    towerConfigs[i].speed       = 100;
    towerConfigs[i].flameLevel  = 255;
  }
}

// Each tower occupies 15 DMX channels: 4 (decoder) + 11 (strobe/uplight).
static const uint8_t CHANNELS_PER_TOWER = 15;

// Accumulator LED strips are RGB-only, old, and power-limited — sending full
// RGB (especially all three for white) overdraws the supply. Cap their colour
// to this percentage. The uplight (modern RGBW) runs at full brightness.
static const uint16_t STRIP_BRIGHTNESS_PCT = 50;

// Write state to one tower. Each tower has TWO fixtures sharing one config:
//   Decoder  (4ch): CH1-3 = accumulator strip RGB (capped); CH4 = FIRE valve only.
//   Uplight (11ch LaluceNatz strobe): full theme RGB + white channel (CH11).
// Fire (decoder CH4) and white (uplight CH11) are independent: firing never
// lights the white channel, and white never opens the valve.
//
// Layout (matches the DMX address labels shown in the web UI):
//   Confluence (central solenoid): CH 1..4              (CH 4 = central valve)
//   Tower 0: decoder A005..A008 (fire=A008), uplight A009..A019
//   Tower 1: decoder A020..A023 (fire=A023), uplight A024..A034
//   Tower 2: decoder A035..A038 (fire=A038), uplight A039..A049
//   Tower 3: decoder A050..A053 (fire=A053), uplight A054..A064
//
// NOTE: DMX addresses are 1-indexed. Writing to address 0 crashes the bus.
void towerWrite(uint8_t index, const TowerState& state) {
  const uint16_t base = 4 + (uint16_t)index * CHANNELS_PER_TOWER;

  // --- Accumulator decoder: RGB strips (capped) + fire valve on CH4 ---
  dmxShadowWrite((uint8_t)(state.r * STRIP_BRIGHTNESS_PCT / 100), base + 1);  // CH1: Red
  dmxShadowWrite((uint8_t)(state.g * STRIP_BRIGHTNESS_PCT / 100), base + 2);  // CH2: Green
  dmxShadowWrite((uint8_t)(state.b * STRIP_BRIGHTNESS_PCT / 100), base + 3);  // CH3: Blue
  dmxShadowWrite(state.fire,                                      base + 4);  // CH4: FIRE valve

  // --- 11-channel uplight (LaluceNatz LL960S in 11ch mode): full RGB + white ---
  const uint16_t s = base + 4;  // uplight block starts after decoder
  dmxShadowWrite(state.masterDim, s + 1);  // CH1: master dimmer
  dmxShadowWrite(state.rgbStrobe, s + 2);  // CH2: RGB strobe speed
  dmxShadowWrite(0,               s + 3);  // CH3: RGB mode (0 = direct colour)
  dmxShadowWrite(0,               s + 4);  // CH4: RGB mode speed (unused)
  dmxShadowWrite(state.r,         s + 5);  // CH5: Red (full)
  dmxShadowWrite(state.g,         s + 6);  // CH6: Green (full)
  dmxShadowWrite(state.b,         s + 7);  // CH7: Blue (full)
  dmxShadowWrite(state.wStrobe,   s + 8);  // CH8: white strobe speed
  dmxShadowWrite(0,               s + 9);  // CH9: white mode (0 = direct)
  dmxShadowWrite(0,               s + 10); // CH10: white mode speed (unused)
  dmxShadowWrite(state.white,     s + 11); // CH11: white dimmer (independent of fire)
}

void towersWrite(const TowerState& state) {
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    towerWrite(i, state);
  }
  dmxDevice.update();
}
