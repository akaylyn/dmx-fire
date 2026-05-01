#pragma once
#include <Arduino.h>

enum FsmState : uint8_t { FSM_IDLE, FSM_FIRE_ACTIVE, FSM_END_CUE, FSM_COOLDOWN };

struct ButtonConfig {
  uint8_t  mode;            // 0=FIREBALL, 1=PARTY
  uint16_t fireDurationMs;  // max solenoid open time (default 3000 ms)
  uint16_t cooldownMs;      // lockout before next trigger (default 10000 ms)
  uint8_t  endCuePattern;   // 0=white flash fade, 1=colour cascade
};

extern ButtonConfig buttonConfig;
extern FsmState     fsmState;

void     buttonFsmSetup();
// Pass the one-shot wasPressed/wasReleased flags and current held state.
void     buttonFsmTick(bool wasPressed, bool wasReleased, bool isHeld);
uint32_t fsmElapsedMs();
