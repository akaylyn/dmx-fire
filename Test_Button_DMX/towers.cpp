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

// Stride between tower blocks. Only the first 8 channels of each stride are
// claimed by a fixture — 4 (decoder) + 4 (uplight in 4-channel mode) — but the
// stride stays at 15 so every fixture keeps the start address it already has.
// Switching the uplights from 11-channel to 4-channel mode therefore needs no
// re-addressing in the field. See docs/spec-uplight-4ch-mode.md.
static const uint8_t CHANNELS_PER_TOWER = 15;

// Accumulator LED strips are RGB-only, old, and power-limited — sending full
// RGB (especially all three for white) overdraws the supply. Cap their colour
// to this percentage; full white (all three channels) is the worst case, so
// keeping this at/under the safe-white ceiling protects every theme. The
// uplight (modern RGBW) runs at full brightness, uncapped.
static const uint16_t STRIP_BRIGHTNESS_PCT = 75;

// Write state to one tower. Each tower has TWO fixtures sharing one config:
//   Decoder (4ch): CH1-3 = accumulator strip RGB (capped); CH4 = FIRE valve only.
//   Uplight (4ch): LaluceNatz LL960S in 4-channel mode — R, G, B, W direct.
// The two fixtures take their colour from different fields (r/g/b vs ur/ug/ub),
// so the uplight can go to the fire look while the strips stay on the theme.
// Fire (decoder CH4) and white (uplight CH4) remain independent channels: the
// valve byte never lights white, and white never opens the valve.
//
// Layout (matches the DMX address labels shown in the web UI):
//   Confluence (central solenoid): CH 1..4              (CH 1 = central valve)
//   Tower 0: decoder A005..A008 (fire=A008), uplight A009..A012, CH 13..19 unclaimed
//   Tower 1: decoder A020..A023 (fire=A023), uplight A024..A027, CH 28..34 unclaimed
//   Tower 2: decoder A035..A038 (fire=A038), uplight A039..A042, CH 43..49 unclaimed
//   Tower 3: decoder A050..A053 (fire=A053), uplight A054..A057, CH 58..64 unclaimed
//
// NOTE: DMX addresses are 1-indexed. Writing to address 0 crashes the bus.
void towerWrite(uint8_t index, const TowerState& state) {
  const uint16_t base = 4 + (uint16_t)index * CHANNELS_PER_TOWER;

  // --- Accumulator decoder: RGB strips (capped) + fire valve on CH4 ---
  dmxShadowWrite((uint8_t)(state.r * STRIP_BRIGHTNESS_PCT / 100), base + 1);  // CH1: Red
  dmxShadowWrite((uint8_t)(state.g * STRIP_BRIGHTNESS_PCT / 100), base + 2);  // CH2: Green
  dmxShadowWrite((uint8_t)(state.b * STRIP_BRIGHTNESS_PCT / 100), base + 3);  // CH3: Blue
  dmxShadowWrite(state.fire,                                      base + 4);  // CH4: FIRE valve

  // --- Uplight (LaluceNatz LL960S, 4-channel mode): full RGB + white ---
  // 4-channel mode is plain linear dimming per colour: no master dimmer and no
  // strobe gate to open, so brightness must already be baked into the state
  // values (themeRender() does this) and there is nothing else to send.
  // Uplight RGB comes from ur/ug/ub, NOT r/g/b: while a valve is open the main
  // loop swaps the uplight to the fire look and leaves the strips on the theme.
  const uint16_t s = base + 4;  // uplight block starts after the decoder
  dmxShadowWrite(state.ur,    s + 1);  // CH1: Red (full, uncapped)
  dmxShadowWrite(state.ug,    s + 2);  // CH2: Green
  dmxShadowWrite(state.ub,    s + 3);  // CH3: Blue
  dmxShadowWrite(state.white, s + 4);  // CH4: White (independent of the valve)

  // --- Unclaimed tail of the stride ---
  // No fixture listens here in 4-channel mode. Drive it to 0 anyway so these
  // channels can never hold a stale nonzero byte next to a valve channel.
  for (uint16_t c = s + 5; c <= base + CHANNELS_PER_TOWER; c++) {
    dmxShadowWrite(0, c);
  }
}

void towersWrite(const TowerState& state) {
  for (uint8_t i = 0; i < NUM_TOWERS; i++) {
    towerWrite(i, state);
  }
  dmxUpdate();
}
