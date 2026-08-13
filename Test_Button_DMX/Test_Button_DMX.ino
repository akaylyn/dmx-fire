/*
 * @Hardwares: M5AtomS3 Lite + Unit DMX
 * @Platform Version: Arduino M5Stack Board Manager v2.1.3
 * @Dependent Library:
 * M5Unified@^0.2.11: https://github.com/m5stack/M5Unified
 * FastLED@^3.9.10: https://github.com/FastLED/FastLED
 * DMX512 frames are emitted directly on Serial1 by dmx.cpp (see dmx.h for why
 * SparkFunDMX::update() is no longer used).
 */

#include <M5Unified.h>
#include <FastLED.h>
#include "themes.h"
#include "dmx.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "storage.h"
#include "web.h"
#include "log.h"
#include "morse.h"
#include "tests.h"

#define KEY_INPUT_PIN 39  // simple switch
m5::Button_Class keyButton;

#define ATOM_RGB_PIN 35  // one-wire data line
CRGB ATOM_LED[1];

// Swap the uplight over to the configured fire look. Touches ONLY the uplight
// fields — the accumulator strips (r/g/b) keep running the theme underneath, so
// firing changes what the uplight shows without disturbing the strips.
// White is assigned rather than max()'d: the fire look owns the uplight while a
// valve is open, and the end-cue fade runs afterwards, never at the same time.
static inline void applyFireLook(TowerState& s) {
  s.ur    = buttonConfig.fireUpR;
  s.ug    = buttonConfig.fireUpG;
  s.ub    = buttonConfig.fireUpB;
  s.white = buttonConfig.fireUpW;
}

void setup() {
  delay(500);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Ex_I2C.release();  // Free up PORTA so we can use it for Serial1

  Serial.print("\nStartup.\n");

  pinMode(KEY_INPUT_PIN, INPUT_PULLUP);
  pinMode(ATOM_RGB_PIN, OUTPUT);

  FastLED.addLeds<WS2812, ATOM_RGB_PIN, GRB>(ATOM_LED, 1);
  ATOM_LED[0] = CRGB::Red;
  FastLED.setBrightness(255);
  FastLED.show();

  dmxSetup();
  towerSetup();
  confluenceSetup();
  buttonFsmSetup();
  storageLoad();  // overwrite defaults with persisted config before web UI starts
  webSetup();

  runDiagnostics();  // prints pass/fail report to serial; also runs a 500 ms DMX visual test
}

void loop() {
  M5.update();
  webTick();
  FastLED.show();

  bool pressed = (digitalRead(KEY_INPUT_PIN) == LOW);
  keyButton.setRawState(millis(), pressed);

  // No keepalive here: the 50 Hz frame loop below is the single DMX writer. A
  // second writer could call update() while a frame was still in the shift
  // register, and the baud-rate change for the next break would mangle its tail.

  // OR physical events with API-injected events so the FSM is single-sourced.
  bool btnPressed  = keyButton.wasPressed()  || buttonConsumePress();
  bool btnReleased = keyButton.wasReleased() || buttonConsumeRelease();
  bool btnHeld     = keyButton.isPressed()   || buttonVirtualHeld();
  if (btnPressed)  LOG_I("[BTN] pressed");
  if (btnReleased) LOG_I("[BTN] released");
  buttonFsmTick(btnPressed, btnReleased, btnHeld);

  // DMX output. Interval lives in dmx.h — a ~3.1 ms frame leaves the rest of the
  // interval idle, so lowering it shortens the idle window on the bus.
  EVERY_N_MILLISECONDS(DMX_FRAME_INTERVAL_MS) {
    // Purge overlay: held-open "empty the accumulator" action, independent of
    // the FSM. Bypasses fireDurationMs and cooldown entirely.
    bool purge = purgeActive();

    // Fire gate for this frame. fsmConsumeFirePending() drains a latch, so it is
    // called EXACTLY ONCE per frame, before the tower loop — that way all five
    // valves (four towers + Confluence) agree on the same frame, and a
    // FIRE_ACTIVE window shorter than one frame still reaches the wire.
    //
    // Drained into its own variable rather than written inline as
    // `(fsmState == FSM_FIRE_ACTIVE) || fsmConsumeFirePending()`: `||`
    // short-circuits, so during a normal burn the latch would never be consumed
    // and would then fire a spurious extra frame of valve-open after the FSM had
    // already left FIRE_ACTIVE.
    bool firePending = fsmConsumeFirePending();
    bool firing = (fsmState == FSM_FIRE_ACTIVE) || firePending;

    // MACHINE_GUN pulses every valve in lockstep — towers and Confluence alike.
    // Off-time is one DMX frame, the shortest gap this bus can express.
    bool mgOn = true;
    if (buttonConfig.mode == 2) {
      uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + DMX_FRAME_INTERVAL_MS;
      mgOn = (millis() % period) < (uint32_t)buttonConfig.machineGunBurstMs;
    }

    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      if (!towerConfigs[i].connected) continue;

      // Theme renderer owns the per-frame colour + uplight white in every state,
      // for the strips (r/g/b) and the uplight (ur/ug/ub) alike. The fire look
      // and the end-cue white flash are overlaid on top: the fire look moves only
      // the uplight, so the strips keep running the theme underneath, and the
      // white flash still never opens a valve.
      TowerState state = themeRender(towerConfigs[i].themeName, i, millis(),
                                     towerConfigs[i].bright, towerConfigs[i].speed);

      if (firing) {
        // Open the per-tower propane valve and light the uplight to match. The
        // uplight stays lit for the whole burn even in MACHINE_GUN mode — only
        // the valve pulses, because strobing the uplight at the burst rate reads
        // as a fault and is a photosensitivity hazard.
        state.fire = mgOn ? towerConfigs[i].flameLevel : 0;
        applyFireLook(state);
      } else if (fsmState == FSM_END_CUE && buttonConfig.endCueMs > 0) {
        // White flash fade on the uplight white channel (not the valve), scaled
        // to the configured end-cue length.
        uint32_t elapsed = fsmElapsedMs();
        uint8_t  fade = (elapsed < (uint32_t)buttonConfig.endCueMs)
                        ? (uint8_t)(255 - elapsed * 255 / buttonConfig.endCueMs)
                        : 0;
        if (fade > state.white) state.white = fade;
      }
      // IDLE / COOLDOWN: theme only.

      // Purge wins over the FSM: hold this tower's accumulator valve fully open
      // and light its uplight, so an open valve is never visually silent.
      if (purge) {
        state.fire = towerConfigs[i].flameLevel;
        applyFireLook(state);
      }

      towerWrite(i, state);
    }

    if (confluenceConfig.connected) {
      uint8_t cfLevel = 0;
      if (purge) {
        cfLevel = confluenceConfig.fireLevel;
      } else if (morseActive()) {
        cfLevel = morseTick();
      } else if (firing) {
        // Same mgOn gate as the tower valves above, so all five fire together.
        cfLevel = mgOn ? confluenceConfig.fireLevel : 0;
      }
      confluenceWrite(cfLevel);
    }

    // Log DMX frame on state transitions so we can verify channel values on the wire.
    static FsmState lastDmxLogState = FSM_IDLE;
    if (fsmState != lastDmxLogState) {
      lastDmxLogState = fsmState;
      LOG_I("[DMX] state=%s CH1(R)=%d CH2(G)=%d CH3(B)=%d CH4(W)=%d CH5(dim)=%d",
            fsmStateName(fsmState),
            dmxLastFrame[0], dmxLastFrame[1], dmxLastFrame[2], dmxLastFrame[3], dmxLastFrame[4]);
    }

    dmxUpdate();
  }

  // Onboard LED reflects FSM state
  EVERY_N_MILLISECONDS(20) {
    static uint8_t idleHue = 0;
    switch (fsmState) {
      case FSM_FIRE_ACTIVE: ATOM_LED[0] = CRGB::Red;                    break;
      case FSM_END_CUE:     ATOM_LED[0] = CRGB::White;                  break;
      case FSM_COOLDOWN:    ATOM_LED[0] = CRGB(255, 100, 0);            break;
      default:              ATOM_LED[0] = CHSV(idleHue++, 255, 255);    break;
    }
  }

  if (M5.BtnA.wasPressed()) {
    LOG_I("[BTN] Atom pressed — running diagnostics");
    runDiagnostics();
  }

  // Heartbeat — current state every 5 s so the log is never silent
  EVERY_N_MILLISECONDS(5000) {
    const char* states[] = {"IDLE", "FIRE_ACTIVE", "END_CUE", "COOLDOWN"};
    LOG_I("[STATUS] fsm=%-11s  uptime=%lu s", states[fsmState], millis() / 1000);
  }
}
