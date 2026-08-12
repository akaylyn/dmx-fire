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

// Frame accounting for troubleshooting — exposed via dmx.h so the main loop can
// print them. sent = frames actually put on the wire; skipped = ticks where the
// previous frame had not finished draining (see dmxReadyToSend()). A rising
// skipped count means the TX path is starving the bus.
uint32_t dmxFramesSent    = 0;
uint32_t dmxFramesSkipped = 0;

// Free space in the TX buffer when the UART is fully idle, captured once in
// dmxSetup() after a flush(). availableForWrite() returns this same value only
// when no frame bytes are still enqueued, so it is the reference dmxReadyToSend()
// compares against — robust to whatever the actual buffer size turns out to be.
static int dmxTxIdleFree = 0;

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
  dmxTxIdleFree = Serial1.availableForWrite();  // baseline: buffer fully drained
}

bool dmxReadyToSend() {
  // Idle iff the TX buffer has as much free space as it did when empty. If a
  // previous frame is still draining, availableForWrite() is lower than the
  // baseline and we report not-ready. Non-blocking.
  return Serial1.availableForWrite() >= dmxTxIdleFree;
}

void dmxUpdate() {
  // Query the UART before starting a new frame: if the previous one has not
  // finished leaving the TX buffer, skip this tick rather than dropping a break
  // on top of bytes still in flight (that baud-rate change is exactly what
  // mangles a frame tail — see dmx.h). The trailing flush() below normally
  // guarantees readiness by the next call, so on the single-writer path this
  // never skips; it is the guard that keeps a second writer, or a future
  // non-blocking pacing loop, from ever colliding mid-frame.
  if (!dmxReadyToSend()) { dmxFramesSkipped++; return; }

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
  dmxFramesSent++;
}
