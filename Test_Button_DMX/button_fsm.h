#pragma once
#include <Arduino.h>

enum FsmState : uint8_t { FSM_IDLE, FSM_FIRE_ACTIVE, FSM_END_CUE, FSM_COOLDOWN };

struct ButtonConfig {
  uint8_t  mode;                // 0=FIREBALL, 1=PARTY, 2=MACHINE_GUN
  uint16_t fireDurationMs;      // max solenoid open time (default 3000 ms)
  uint16_t cooldownMs;          // lockout before next trigger (default 10000 ms)
  uint8_t  endCuePattern;       // 0=white flash fade, 1=colour cascade
  uint16_t machineGunBurstMs;   // solenoid on-time per pulse in MACHINE_GUN mode (default 200 ms)
};

extern ButtonConfig buttonConfig;
extern FsmState     fsmState;

const char* fsmStateName(FsmState s);
void     buttonFsmSetup();
// Pass the one-shot wasPressed/wasReleased flags and current held state.
void     buttonFsmTick(bool wasPressed, bool wasReleased, bool isHeld);
uint32_t fsmElapsedMs();

// --- API-driven (virtual) button injection ---
// Used by /api/button/* HTTP handlers and the web UI Test Fire button.
// Physical and virtual events are OR-merged in the main loop so the FSM
// behaves identically regardless of source.
void buttonInjectPress();    // queue a one-shot press; sticks held=true
void buttonInjectRelease();  // queue a one-shot release; clears held
void buttonInjectReset();    // force FSM back to IDLE (skips cooldown for tests)
bool buttonConsumePress();   // main loop: returns + clears pending press
bool buttonConsumeRelease(); // main loop: returns + clears pending release
bool buttonVirtualHeld();    // sticky held flag from inject press/release

// --- Purge / empty-accumulator overlay ---
// Independent of the FSM: while active, the main loop holds every tower
// accumulator valve AND the central Confluence solenoid fully open, with no
// duration limit and no cooldown, until purgeStop() is called (button release).
// Used by /api/purge/start|stop and the web UI "Empty Accumulator" tab.
void purgeStart();
void purgeStop();
bool purgeActive();
