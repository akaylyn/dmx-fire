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

  // define pin modes after M5.begin(), as it might (kinda randomly) yoink a pin for... stuff.
  pinMode(KEY_INPUT_PIN, INPUT_PULLUP);
  pinMode(ATOM_RGB_PIN, OUTPUT);

  FastLED.addLeds<WS2812, ATOM_RGB_PIN, GRB>(ATOM_LED, 1);
  ATOM_LED[0] = CRGB::Red;
  FastLED.setBrightness(255);
  FastLED.show();

  dmxSetup();
  towerSetup();
  webSetup();
}

void loop() {
  M5.update();
  webTick();
  FastLED.show();

  // This is shite: the constructor can't map to a hardware pin.
  // So, map GPIO → button
  bool pressed = (digitalRead(KEY_INPUT_PIN) == LOW);
  keyButton.setRawState(millis(), pressed);

  dmxKeepalive();

  if (keyButton.wasPressed()) {
    Serial.println("External Pressed");
    ATOM_LED[0] = CRGB::White;
  }

  if (keyButton.wasReleased()) {
    Serial.println("External Released");
  }

  // DMX output — 50 Hz
  EVERY_N_MILLISECONDS(20) {
    static uint8_t currIndex = 0;
    static CRGBPalette256 buttonPal = electricBlueFirePal;
    bool held = keyButton.isPressed();

    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      CRGBPalette256& pal    = held ? buttonPal              : towerConfigs[i].pal;
      uint8_t         bright = held ? (uint8_t)255           : towerConfigs[i].bright;

      CRGB c = ColorFromPalette(pal, currIndex, bright, LINEARBLEND);

      TowerState state;
      state.r         = c.r;
      state.g         = c.g;
      state.b         = c.b;
      state.masterDim = 255;
      state.wDim      = bright;
      state.rgbStrobe = held ? 128 : 0;
      state.wStrobe   = held ? 128 : 0;

      towerWrite(i, state);
    }
    currIndex++;
    dmxDevice.update();
  }

  // Onboard LED cycles through the HSV colorwheel when idle
  if (!keyButton.isPressed()) {
    EVERY_N_MILLISECONDS(20) {
      static uint8_t currHue = 0;
      ATOM_LED[0] = CHSV(currHue++, 255, 255);
    }
  }

  if (M5.BtnA.isPressed()) {
    Serial.println("Atom Pressed");
    ATOM_LED[0] = CRGB::White;
  }

  if (M5.BtnA.wasReleased()) {
    Serial.println("Atom Released");
  }
}
