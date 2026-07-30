#include <Arduino.h>
#include "dmx.h"

static const uint8_t  RX_PIN = 1;
static const uint8_t  TX_PIN = 2;

// DMX512 line rate: 250 kbaud, 8 data bits, no parity, 2 stop bits.
static const uint32_t DMX_BAUD   = 250000;
static const uint32_t DMX_FORMAT = SERIAL_8N2;

// The ESP32 UART cannot assert a real break, so we fake one the standard way:
// drop the baud rate and send a single zero byte. The low pulse is the start bit
// plus 8 zero data bits = 9 bit-times; the 2 stop bits that follow are the
// mark-after-break.
//
//   break = 9 / BREAK_BAUD    MAB = 2 / BREAK_BAUD
//
// At 50000 baud that is a 180 us break and a 40 us MAB. Spec floors are 88 us and
// 8 us, so both have real margin — deliberately, because the previous 99 us break
// (SparkFunDMX's default) was close enough to the floor that a decoder was
// intermittently missing it and resyncing mid-frame. Do not raise BREAK_BAUD back
// toward 90000 without re-testing every decoder on the bus.
static const uint32_t BREAK_BAUD = 50000;

// DMX null start code — identifies this as a dimmer-data packet. Must precede
// channel 1; omitting it shifts every channel by one.
static const uint8_t DMX_START_CODE = 0x00;

uint8_t dmxLastFrame[DMX_FRAME_SLOTS] = {0};

void dmxShadowWrite(uint8_t value, uint16_t ch) {
  // DMX is 1-indexed. Ch 0 is invalid and would corrupt the start code.
  if (ch >= 1 && ch <= DMX_SHADOW_SIZE) {
    dmxLastFrame[ch - 1] = value;
  }
}

void dmxSetup() {
  Serial1.begin(DMX_BAUD, DMX_FORMAT, RX_PIN, TX_PIN);
  Serial1.setTxBufferSize(512);
  Serial1.flush();
}

void dmxUpdate() {
  // BREAK + MAB
  Serial1.updateBaudRate(BREAK_BAUD);
  Serial1.write((uint8_t)0);
  Serial1.flush();  // the break must complete before the baud rate changes back

  // Start code + slot data at the real line rate. Sends DMX_FRAME_SLOTS slots,
  // which is longer than the addressed universe on purpose — see dmx.h.
  Serial1.updateBaudRate(DMX_BAUD);
  Serial1.write(DMX_START_CODE);
  Serial1.write(dmxLastFrame, DMX_FRAME_SLOTS);

  // Block until the frame is fully out. Without this the next call's
  // updateBaudRate() can change the divider mid-byte and mangle the tail.
  Serial1.flush();
}
