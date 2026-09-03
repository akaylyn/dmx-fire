#pragma once
#include <Arduino.h>

struct ConfluenceConfig {
  bool connected;
  // Whether the central solenoid may open at all. Replaces the old fireLevel
  // byte: CH1 drives an on/off valve, so a level between 0 and 255 never made a
  // smaller flame — it only decided whether the decoder's turn-on threshold was
  // cleared. See docs/spec-solenoid-binary.md.
  bool fireEnabled;
};

extern ConfluenceConfig confluenceConfig;

void confluenceSetup();
void confluenceWrite(bool open);  // writes the CH1 valve, then 0 to ch 2–4
