#include <Arduino.h>
#include "dmx.h"

static const uint8_t  RX_PIN      = 1;
static const uint8_t  TX_PIN      = 2;
static const uint8_t  EN_PIN      = (uint8_t)-1;  // 255 = no enable pin
static const uint16_t NUM_CHANNELS = 64;

SparkFunDMX dmxDevice;
uint16_t    dmxSerialBufferSize = 0;
uint8_t     dmxLastFrame[DMX_SHADOW_SIZE] = {0};

void dmxShadowWrite(uint8_t value, uint16_t ch) {
  // DMX is 1-indexed. Ch 0 is invalid and would crash the bus.
  if (ch >= 1 && ch <= DMX_SHADOW_SIZE) {
    dmxLastFrame[ch - 1] = value;
  }
  dmxDevice.writeByte(value, ch);
}

void dmxSetup() {
  Serial1.begin(DMX_BAUD, DMX_FORMAT, RX_PIN, TX_PIN);
  Serial1.setTxBufferSize(512);
  Serial1.flush();
  dmxSerialBufferSize = Serial1.availableForWrite();
  dmxDevice.begin(Serial1, EN_PIN, NUM_CHANNELS);
  dmxDevice.setComDir(DMX_WRITE_DIR);
}

void dmxKeepalive() {
  // Some DMX fixtures time out without a periodic packet; send one per second.
  static unsigned long lastMs = 0;
  unsigned long now = millis();
  if (now - lastMs >= 1000) {
    lastMs = now;
    uint16_t sending = dmxSerialBufferSize - Serial1.availableForWrite();
    if (sending == 0) dmxDevice.update();
  }
}

