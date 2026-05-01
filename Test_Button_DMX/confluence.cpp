#include "dmx.h"
#include "confluence.h"

ConfluenceConfig confluenceConfig;

void confluenceSetup() {
  confluenceConfig.connected = true;
  confluenceConfig.fireLevel = 255;
}

// Writes to Confluence DMX channels 1–4.
// CH1–3 are wired but ignored by the fixture; only CH4 opens the solenoid.
void confluenceWrite(uint8_t level) {
  dmxDevice.writeByte(0,     1);
  dmxDevice.writeByte(0,     2);
  dmxDevice.writeByte(0,     3);
  dmxDevice.writeByte(level, 4);
}
