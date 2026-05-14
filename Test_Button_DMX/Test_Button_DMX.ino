/*
 * @Hardwares: M5AtomS3 Lite + Unit DMX
 * @Platform Version: Arduino M5Stack Board Manager v2.1.3
 * @Dependent Library:
 * M5Unified@^0.2.11: https://github.com/m5stack/M5Unified
 * FastLED@^3.9.10: https://github.com/FastLED/FastLED
 * SparkFunDMX@^2.0.1: https://github.com/sparkfun/SparkFunDMX
 */

#include <M5Unified.h>
#include <FastLED.h>
#include "palettes.h"
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

  dmxKeepalive();

  // OR physical events with API-injected events so the FSM is single-sourced.
  bool btnPressed  = keyButton.wasPressed()  || buttonConsumePress();
  bool btnReleased = keyButton.wasReleased() || buttonConsumeRelease();
  bool btnHeld     = keyButton.isPressed()   || buttonVirtualHeld();
  if (btnPressed)  LOG_I("[BTN] pressed");
  if (btnReleased) LOG_I("[BTN] released");
  buttonFsmTick(btnPressed, btnReleased, btnHeld);

  // DMX output — 50 Hz
  EVERY_N_MILLISECONDS(20) {
    static uint8_t paletteIndex = 0;

    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      if (!towerConfigs[i].connected) continue;

      TowerState state = {};
      state.masterDim  = 255;

      switch (fsmState) {
        case FSM_FIRE_ACTIVE: {
          // White only: channel 4 (W) at full flameLevel, RGB off.
          state.wDim = towerConfigs[i].flameLevel;  // 255 by default; user-adjustable per tower
          break;
        }
        case FSM_END_CUE: {
          uint32_t elapsed = fsmElapsedMs();
          state.wDim = (elapsed < 1000) ? (uint8_t)(255 - elapsed * 255 / 1000) : 0;
          break;
        }
        default: {
          // IDLE / COOLDOWN: blank with an occasional palette-coloured flash.
          // 4-second cycle, 800 ms ON, rest OFF.
          uint32_t cyclePos = millis() % 4000;
          if (cyclePos < 800) {
            CRGB c = ColorFromPalette(towerConfigs[i].pal, paletteIndex, towerConfigs[i].bright, LINEARBLEND);
            state.r    = c.r;
            state.g    = c.g;
            state.b    = c.b;
            state.wDim = towerConfigs[i].bright;
          } else {
            state.r = state.g = state.b = 0;
            state.wDim = 0;
          }
          break;
        }
      }

      towerWrite(i, state);
    }

    if (confluenceConfig.connected) {
      uint8_t cfLevel = 0;
      if (morseActive()) {
        cfLevel = morseTick();
      } else if (fsmState == FSM_FIRE_ACTIVE) {
        if (buttonConfig.mode == 2) {
          // Machine gun: pulse solenoid — on for machineGunBurstMs, off for 50 ms
          uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + 50;
          cfLevel = (millis() % period < (uint32_t)buttonConfig.machineGunBurstMs)
                    ? confluenceConfig.fireLevel : 0;
        } else {
          cfLevel = confluenceConfig.fireLevel;
        }
      }
      confluenceWrite(cfLevel);
    }

    paletteIndex++;

    // Log DMX frame on state transitions so we can verify channel values on the wire.
    static FsmState lastDmxLogState = FSM_IDLE;
    if (fsmState != lastDmxLogState) {
      lastDmxLogState = fsmState;
      LOG_I("[DMX] state=%s CH1(R)=%d CH2(G)=%d CH3(B)=%d CH4(W)=%d CH5(dim)=%d",
            fsmStateName(fsmState),
            dmxLastFrame[0], dmxLastFrame[1], dmxLastFrame[2], dmxLastFrame[3], dmxLastFrame[4]);
    }

    dmxDevice.update();
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
