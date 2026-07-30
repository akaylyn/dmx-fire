#include "dmx.h"
#include "confluence.h"

ConfluenceConfig confluenceConfig;

void confluenceSetup() {
  confluenceConfig.connected = true;
  confluenceConfig.fireLevel = 255;
}

// Writes the Confluence block, DMX channels 1–4.
//
// The Confluence decoder is a 3-channel RGB unit addressed A001, so it listens
// on CH1–3 only, and the propane solenoid is wired to its first output:
//   CH1 = solenoid   CH2/CH3 = decoder outputs 2 and 3, nothing wired
//
// CH4 is outside the decoder's span and belongs to no fixture (Tower 0 starts
// at A005). It is still driven to 0 every frame so an unclaimed channel can
// never hold a stale nonzero byte — on a bus with the noise issues this rig has
// seen, an undriven channel next to a valve is not worth the risk.
// See docs/spec-confluence-addressing.md.
void confluenceWrite(uint8_t level) {
  dmxShadowWrite(level, 1);  // solenoid
  dmxShadowWrite(0,     2);
  dmxShadowWrite(0,     3);
  dmxShadowWrite(0,     4);  // unclaimed — parked at 0
}
