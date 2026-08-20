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

// Sticky "a fire was triggered" latch. Set on every entry to FIRE_ACTIVE and
// drained once per DMX frame by fsmConsumeFirePending(), so a FIRE_ACTIVE window
// shorter than one frame still puts the valve open on the wire for one frame.
static volatile bool g_firePending    = false;

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
  // Latch the trigger so a sub-frame FIRE_ACTIVE window still reaches the bus.
  if (s == FSM_FIRE_ACTIVE) g_firePending = true;
}

// Where FIRE_ACTIVE goes when it ends. endCueMs == 0 skips END_CUE entirely
// rather than passing through it for one tick, so /api/state never shows a
// one-frame END_CUE blip and rapid retrigger is not gated on an extra state.
static FsmState afterFireState() {
  return (buttonConfig.endCueMs == 0) ? FSM_COOLDOWN : FSM_END_CUE;
}

void buttonFsmSetup() {
  buttonConfig.mode              = 0;
  buttonConfig.fireDurationMs    = 3000;
  buttonConfig.cooldownMs        = 10000;
  buttonConfig.endCuePattern     = 0;
  buttonConfig.endCueMs          = 1000;
  buttonConfig.machineGunBurstMs = 200;
  buttonConfig.fireUpR           = 255;  // amber
  buttonConfig.fireUpG           = 110;
  buttonConfig.fireUpB           = 0;
  buttonConfig.fireUpW           = 0;    // white off by default — keeps the amber saturated
  enterState(FSM_IDLE);
}

uint32_t fsmElapsedMs() {
  return millis() - stateEnteredMs;
}

bool fsmConsumeFirePending() {
  if (g_firePending) { g_firePending = false; return true; }
  return false;
}

void buttonFsmTick(bool wasPressed, bool wasReleased, bool isHeld) {
  uint32_t elapsed = fsmElapsedMs();

  switch (fsmState) {
    case FSM_IDLE:
      if (wasPressed) enterState(FSM_FIRE_ACTIVE);
      break;

    case FSM_FIRE_ACTIVE:
      if (elapsed >= (uint32_t)buttonConfig.fireDurationMs) {
        enterState(afterFireState());
      }
      // Close on release. FIREBALL (0) runs its full duration; PARTY (1) and
      // MACHINE_GUN (2) stop when the operator lets go; every audio mode (3–6) is
      // release-terminated too, because audio.cpp owns shot length via the limiter
      // grant and needs to be able to stop a shot early.
      if (modeClosesOnRelease(buttonConfig.mode) && wasReleased) {
        enterState(afterFireState());
      }
      break;

    case FSM_END_CUE:
      if (elapsed >= (uint32_t)buttonConfig.endCueMs) enterState(FSM_COOLDOWN);
      break;

    case FSM_COOLDOWN:
      if (elapsed >= (uint32_t)buttonConfig.cooldownMs) enterState(FSM_IDLE);
      break;
  }
}

// End a burn now, regardless of mode. Only ever CLOSES — the only transition it can
// make is FIRE_ACTIVE -> afterFireState(), never into FIRE_ACTIVE.
void fsmEndFireNow() {
  if (fsmState == FSM_FIRE_ACTIVE) enterState(afterFireState());
}

// Cut a post-fire lockout short. Can ONLY leave END_CUE or COOLDOWN early — it can
// never open a valve, and it deliberately does not touch g_firePending, so a frame of
// valve-open that is still owed is never swallowed (unlike buttonInjectReset()).
//
// Audio modes need this because cooldownMs is tuned for manual fire and would swallow
// every beat. audio.cpp scopes the call to lockouts left by shots audio itself
// started, so the operator's physical button keeps its lockout either way.
void fsmSkipCooldown() {
  if (fsmState == FSM_END_CUE || fsmState == FSM_COOLDOWN) enterState(FSM_IDLE);
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
  // Drop any queued events so the FSM doesn't immediately retrigger. The fire
  // latch goes too — a reset must not leave a frame of valve-open owed.
  g_pendingPress   = false;
  g_pendingRelease = false;
  g_virtualHeld    = false;
  g_firePending    = false;
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
