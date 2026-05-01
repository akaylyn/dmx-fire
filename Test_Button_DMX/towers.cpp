#include <Arduino.h>
#include "dmx.h"
#include "palettes.h"
#include "towers.h"

TowerConfig towerConfigs[NUM_TOWERS];
bool        towerConfigUpdated = false;

void towerSetup() {
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    towerConfigs[i].connected   = true;
    towerConfigs[i].palName     = "green";
    towerConfigs[i].pal         = electricGreenFirePal;
    towerConfigs[i].bright      = 16;
    towerConfigs[i].flameLevel  = 0;
  }
}

// Each tower occupies 15 DMX channels: 4 (decoder) + 11 (strobe).
static const uint8_t CHANNELS_PER_TOWER = 15;

// Write state to one tower.
// Universe layout: ch 1–4 = Confluence, ch 5+ = towers.
// NOTE: DMX addresses are 1-indexed. Writing to address 0 crashes the bus.
void towerWrite(uint8_t index, const TowerState& state) {
  const uint16_t base = 4 + index * CHANNELS_PER_TOWER;

  // --- 4-channel RGBW decoder ---
  dmxDevice.writeByte(state.r,    base + 1);  // CH1: Red
  dmxDevice.writeByte(state.g,    base + 2);  // CH2: Green
  dmxDevice.writeByte(state.b,    base + 3);  // CH3: Blue
  dmxDevice.writeByte(state.wDim, base + 4);  // CH4: White

  // --- 11-channel strobe (LaluceNatz LL960S in 11ch mode) ---
  const uint16_t s = base + 4;  // strobe block starts after decoder
  dmxDevice.writeByte(state.masterDim, s + 1);  // CH1: master dimmer
  dmxDevice.writeByte(state.rgbStrobe, s + 2);  // CH2: RGB strobe speed
  dmxDevice.writeByte(0,               s + 3);  // CH3: RGB mode (0 = direct colour)
  dmxDevice.writeByte(0,               s + 4);  // CH4: RGB mode speed (unused)
  dmxDevice.writeByte(state.r,         s + 5);  // CH5: Red
  dmxDevice.writeByte(state.g,         s + 6);  // CH6: Green
  dmxDevice.writeByte(state.b,         s + 7);  // CH7: Blue
  dmxDevice.writeByte(state.wStrobe,   s + 8);  // CH8: white strobe speed
  dmxDevice.writeByte(0,               s + 9);  // CH9: white mode (0 = direct)
  dmxDevice.writeByte(0,               s + 10); // CH10: white mode speed (unused)
  dmxDevice.writeByte(state.wDim,      s + 11); // CH11: white dimmer
}

void towersWrite(const TowerState& state) {
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    towerWrite(i, state);
  }
  dmxDevice.update();
}
