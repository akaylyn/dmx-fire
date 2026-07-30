#pragma once
#include <Arduino.h>

struct ConfluenceConfig {
  bool    connected;
  uint8_t fireLevel;  // 0=off, 255=full open; written to ch 1 (solenoid)
};

extern ConfluenceConfig confluenceConfig;

void confluenceSetup();
void confluenceWrite(uint8_t level);  // writes level,0,0,0 to ch 1–4
