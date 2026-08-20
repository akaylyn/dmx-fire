#pragma once
#include <Arduino.h>

enum FsmState : uint8_t { FSM_IDLE, FSM_FIRE_ACTIVE, FSM_END_CUE, FSM_COOLDOWN };

struct ButtonConfig {
  uint8_t  mode;                // 0=FIREBALL, 1=PARTY, 2=MACHINE_GUN
  uint16_t fireDurationMs;      // max solenoid open time (default 3000 ms)
  uint16_t cooldownMs;          // lockout before next trigger (default 10000 ms)
  uint8_t  endCuePattern;       // 0=white flash fade; 1=colour cascade is NOT implemented
  uint16_t endCueMs;            // END_CUE duration (default 1000 ms); 0 skips the state entirely
  uint16_t machineGunBurstMs;   // solenoid on-time per pulse in MACHINE_GUN mode (default 200 ms)

  // Uplight colour held while a valve is open (FIRE_ACTIVE or purge). Global for
  // the whole rig, not per tower. Default is amber with the white channel off.
  // See docs/spec-fire-uplight.md.
  uint8_t  fireUpR, fireUpG, fireUpB;
  uint8_t  fireUpW;             // uplight white level during fire (default 0)
};

extern ButtonConfig buttonConfig;
extern FsmState     fsmState;

const char* fsmStateName(FsmState s);
void     buttonFsmSetup();
// Pass the one-shot wasPressed/wasReleased flags and current held state.
void     buttonFsmTick(bool wasPressed, bool wasReleased, bool isHeld);
uint32_t fsmElapsedMs();

// True if FIRE_ACTIVE was entered since the last call; clears the latch.
//
// The DMX block samples fsmState once per 50 ms frame (DMX_FRAME_INTERVAL_MS).
// With a short fireDurationMs the whole FIRE_ACTIVE window can fall between two
// frames, so a fast tap would command nothing and produce no fire at all. The
// main loop ORs this latch into its `firing` flag so every trigger gets at least
// one frame with the valve open — the shortest pulse this bus can express.
// Call exactly once per frame. See docs/spec-rapid-retrigger.md.
bool     fsmConsumeFirePending();

// Which modes stop firing when the button is released. FIREBALL (0) runs its full
// fireDurationMs; everything else closes on release. Audio modes 3–6 are included so
// audio.cpp can end a shot when its limiter grant expires.
static inline bool modeClosesOnRelease(uint8_t m) {
  return m == 1 || m == 2 || (m >= 3 && m <= 6);
}

// End an in-progress burn immediately, whatever the mode is set to. Can ONLY close a
// valve: it moves FIRE_ACTIVE to its normal post-fire state and can never enter it.
//
// A release edge is NOT sufficient. modeClosesOnRelease() is false for FIREBALL, so
// an operator switching to mode 0 mid-burn would have audio's release ignored and the
// valve would stay open for the rest of fireDurationMs.
void fsmEndFireNow();

// Leave END_CUE or COOLDOWN early. Cannot open a valve and does not touch the fire
// latch. Used only by the audio path, and only for lockouts left by its own shots.
void fsmSkipCooldown();

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
