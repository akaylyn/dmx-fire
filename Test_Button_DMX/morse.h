#pragma once
#include <Arduino.h>

// Morse-code fire playback. While active, takes priority over the FSM and
// drives the Confluence solenoid (CH4) directly.
//
// International Morse timing in "units":
//   dot = 1 unit ON     dash = 3 units ON
//   intra-letter gap = 1 unit OFF
//   inter-letter gap = 3 units OFF
//   inter-word  gap = 7 units OFF

extern uint16_t morseUnitMs;   // duration of one Morse unit, default 150 ms

bool    morseStart(const String& text);  // returns false if text has no codable chars
void    morseStop();
bool    morseActive();
uint8_t morseTick();           // returns desired CH4 level (0 or fireLevel)
