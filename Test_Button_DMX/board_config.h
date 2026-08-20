#pragma once
#include <Arduino.h>

// Per-board pin map and feature flags.
//
// The rig runs on an M5AtomS3 Lite. Other ESP32-S3 boards are supported only as
// BENCH TARGETS for exercising the upload paths (USB flash and OTA) when no
// spare AtomS3 is available — see docs/spec-upload-targets.md. They are not
// wired to the fire hardware and are not a supported deployment target.
//
// Selected by the Arduino board macro, which comes from `build.board` in
// boards.txt: m5stack_atoms3 -> ARDUINO_M5STACK_ATOMS3,
//             m5stack_cores3 -> ARDUINO_M5STACK_CORES3.

#if defined(ARDUINO_M5STACK_CORES3)

  #define BOARD_NAME "M5CoreS3 (bench target)"

  // CoreS3 has an LCD, not an addressable status LED. GPIO35 is not a NeoPixel
  // here, so the FastLED status indicator is compiled out entirely.
  #define HAS_STATUS_LED 0

  // CoreS3 has no button on GPIO39. Left enabled, a floating input read as LOW
  // would look like a held button and drive the FSM straight into FIRE_ACTIVE
  // on repeat. The physical button is compiled out; the API/web Test Fire path
  // still works, which is what the upload tests need.
  #define HAS_PHYSICAL_BUTTON 0

  // Port A is G1/G2 on both boards, so DMX pinout is unchanged. Nothing is
  // wired to it on a bench target — frames go out to nobody.
  #define DMX_RX_PIN 1
  #define DMX_TX_PIN 2

#else  // default: ARDUINO_M5STACK_ATOMS3 — the real rig

  #define BOARD_NAME "M5AtomS3 Lite"
  #define HAS_STATUS_LED      1
  #define HAS_PHYSICAL_BUTTON 1

  #define STATUS_LED_PIN 35   // one-wire WS2812 data line
  #define BUTTON_PIN     39   // simple switch to ground

  #define DMX_RX_PIN 1
  #define DMX_TX_PIN 2

#endif
