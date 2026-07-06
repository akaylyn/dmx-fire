#include "button_fsm.h"
#include "log.h"

ButtonConfig buttonConfig;
FsmState     fsmState      = FSM_IDLE;

static uint32_t stateEnteredMs = 0;

// API-injected button events. Set by buttonInjectPress/Release, consumed
// once per loop iteration in Test_Button_DMX.ino's main tick.
static volatile bool g_pendingPress   = false;
static volatile bool g_pendingRelease = false;
static volatile bool g_virtualHeld    = false;

// Purge overlay flag — set by purgeStart(), cleared by purgeStop(). Read every
// frame in the main loop to force all valves open, bypassing the FSM entirely.
static volatile bool g_purgeActive    = false;

const char* fsmStateName(FsmState s) {
  switch (s) {
    case FSM_IDLE:        return "IDLE";
    case FSM_FIRE_ACTIVE: return "FIRE_ACTIVE";
    case FSM_END_CUE:     return "END_CUE";
    case FSM_COOLDOWN:    return "COOLDOWN";
    default:              return "UNKNOWN";
  }
}

static void enterState(FsmState s) {
  LOG_I("[FSM] %s → %s  (after %lu ms)", fsmStateName(fsmState), fsmStateName(s), millis() - stateEnteredMs);
  fsmState       = s;
  stateEnteredMs = millis();
}

void buttonFsmSetup() {
  buttonConfig.mode              = 0;
  buttonConfig.fireDurationMs    = 3000;
  buttonConfig.cooldownMs        = 10000;
  buttonConfig.endCuePattern     = 0;
  buttonConfig.machineGunBurstMs = 200;
  enterState(FSM_IDLE);
}

uint32_t fsmElapsedMs() {
  return millis() - stateEnteredMs;
}

void buttonFsmTick(bool wasPressed, bool wasReleased, bool isHeld) {
  uint32_t elapsed = fsmElapsedMs();

  switch (fsmState) {
    case FSM_IDLE:
      if (wasPressed) enterState(FSM_FIRE_ACTIVE);
      break;

    case FSM_FIRE_ACTIVE:
      if (elapsed >= (uint32_t)buttonConfig.fireDurationMs) {
        enterState(FSM_END_CUE);
      }
      // PARTY / MACHINE_GUN: also close solenoid on release (FIREBALL runs full duration)
      if ((buttonConfig.mode == 1 || buttonConfig.mode == 2) && wasReleased) {
        enterState(FSM_END_CUE);
      }
      break;

    case FSM_END_CUE:
      if (elapsed >= 1000) enterState(FSM_COOLDOWN);
      break;

    case FSM_COOLDOWN:
      if (elapsed >= (uint32_t)buttonConfig.cooldownMs) enterState(FSM_IDLE);
      break;
  }
}

// --- Virtual button injection (API / web UI Test Fire) ---

void buttonInjectPress() {
  g_pendingPress = true;
  g_virtualHeld  = true;
  LOG_I("[BTN] API press injected");
}

void buttonInjectRelease() {
  g_pendingRelease = true;
  g_virtualHeld    = false;
  LOG_I("[BTN] API release injected");
}

void buttonInjectReset() {
  // Drop any queued events so the FSM doesn't immediately retrigger.
  g_pendingPress   = false;
  g_pendingRelease = false;
  g_virtualHeld    = false;
  enterState(FSM_IDLE);
  LOG_I("[BTN] API reset → FSM_IDLE");
}

bool buttonConsumePress() {
  if (g_pendingPress) { g_pendingPress = false; return true; }
  return false;
}

bool buttonConsumeRelease() {
  if (g_pendingRelease) { g_pendingRelease = false; return true; }
  return false;
}

bool buttonVirtualHeld() {
  return g_virtualHeld;
}

// --- Purge / empty-accumulator overlay ---

void purgeStart() {
  g_purgeActive = true;
  LOG_I("[PURGE] start — all valves held open");
}

void purgeStop() {
  g_purgeActive = false;
  LOG_I("[PURGE] stop — valves closed");
}

bool purgeActive() {
  return g_purgeActive;
}
