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
}

void loop() {
  M5.update();
  webTick();
  FastLED.show();

  bool pressed = (digitalRead(KEY_INPUT_PIN) == LOW);
  keyButton.setRawState(millis(), pressed);

  dmxKeepalive();

  buttonFsmTick(keyButton.wasPressed(), keyButton.wasReleased(), keyButton.isPressed());

  // DMX output — 50 Hz
  EVERY_N_MILLISECONDS(20) {
    static uint8_t         paletteIndex = 0;
    static CRGBPalette256  firePal      = firepal;

    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      if (!towerConfigs[i].connected) continue;

      TowerState state = {};
      state.masterDim  = 255;

      switch (fsmState) {
        case FSM_FIRE_ACTIVE: {
          CRGB c = ColorFromPalette(firePal, paletteIndex, 255, LINEARBLEND);
          state.r         = c.r;
          state.g         = c.g;
          state.b         = c.b;
          state.wDim      = towerConfigs[i].flameLevel;
          state.rgbStrobe = 64;
          break;
        }
        case FSM_END_CUE: {
          uint32_t elapsed = fsmElapsedMs();
          state.wDim = (elapsed < 1000) ? (uint8_t)(255 - elapsed * 255 / 1000) : 0;
          break;
        }
        default: {
          CRGB c = ColorFromPalette(towerConfigs[i].pal, paletteIndex, towerConfigs[i].bright, LINEARBLEND);
          state.r    = c.r;
          state.g    = c.g;
          state.b    = c.b;
          state.wDim = towerConfigs[i].bright;
          break;
        }
      }

      towerWrite(i, state);
    }

    if (confluenceConfig.connected) {
      confluenceWrite(fsmState == FSM_FIRE_ACTIVE ? confluenceConfig.fireLevel : 0);
    }

    paletteIndex++;
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

  if (M5.BtnA.wasPressed())  Serial.println("Atom Pressed");
  if (M5.BtnA.wasReleased()) Serial.println("Atom Released");
}
