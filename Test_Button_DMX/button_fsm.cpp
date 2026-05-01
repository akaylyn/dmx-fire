#include "button_fsm.h"
#include "log.h"

ButtonConfig buttonConfig;
FsmState     fsmState      = FSM_IDLE;

static uint32_t stateEnteredMs = 0;

static const char* stateName(FsmState s) {
  switch (s) {
    case FSM_IDLE:        return "IDLE";
    case FSM_FIRE_ACTIVE: return "FIRE_ACTIVE";
    case FSM_END_CUE:     return "END_CUE";
    case FSM_COOLDOWN:    return "COOLDOWN";
    default:              return "UNKNOWN";
  }
}

static void enterState(FsmState s) {
  LOG_I("[FSM] %s → %s  (after %lu ms)", stateName(fsmState), stateName(s), millis() - stateEnteredMs);
  fsmState       = s;
  stateEnteredMs = millis();
}

void buttonFsmSetup() {
  buttonConfig.mode           = 0;
  buttonConfig.fireDurationMs = 3000;
  buttonConfig.cooldownMs     = 10000;
  buttonConfig.endCuePattern  = 0;
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
      // PARTY mode: also close solenoid on release (FIREBALL runs full duration)
      if (buttonConfig.mode == 1 && wasReleased) {
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
