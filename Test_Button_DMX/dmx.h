#pragma once
#include <Arduino.h>

// DMX512 transmit. Frames are emitted directly onto Serial1 rather than through
// SparkFunDMX::update(), for two reasons found while chasing tower-1 flicker:
//
//   1. The library's break is 99 us — 11 us over the 88 us spec floor. Cheap
//      decoders miss a break that short, fail to resync, and start sampling
//      mid-frame, so their channels pick up whatever data sits at the slipped
//      offset. A valve channel doing that opens propane on colour data.
//   2. The library never flush()es after writing the frame, so the next call's
//      updateBaudRate() can land while bytes are still in the shift register and
//      corrupt the tail of the frame in flight.
//
// See docs/spec-dmx-transmit.md and notes.md.

// Channels that actually address fixtures — the universe laid out in CLAUDE.md.
// dmxShadowWrite() accepts 1..this and nothing else, and /api/state exposes
// exactly this many channels (tests/test_state.py asserts the count).
static const uint16_t DMX_SHADOW_SIZE = 64;

// Slots transmitted per frame. Deliberately longer than the addressed universe: a
// 64-slot frame is very short — a console sends 512 — and cheap decoders can
// misbehave on short packets, which is one of the remaining differences between
// this controller and the manual console that reads the bus cleanly. Slots beyond
// DMX_SHADOW_SIZE are always zero, so the padding can never drive a fixture.
static const uint16_t DMX_FRAME_SLOTS = 128;

// The frame buffer IS what gets transmitted — there is no second copy to keep in
// sync. Index 0 = DMX channel 1. The first DMX_SHADOW_SIZE entries are live
// channel data; the rest is zero padding.
extern uint8_t dmxLastFrame[DMX_FRAME_SLOTS];

// Frame interval in ms. A 128-slot frame (break + start code + 128 slots) occupies
// ~5.9 ms on the wire, so the line idles for the remainder of each interval —
// lowering this shortens that idle window, raising it lengthens it.
static const uint8_t DMX_FRAME_INTERVAL_MS = 50;  // 20 Hz — ~44 ms idle per frame

void dmxSetup();

// Write a byte into the frame buffer. 1-indexed; channels outside
// 1..DMX_SHADOW_SIZE are ignored so the padding stays zero.
void dmxShadowWrite(uint8_t value, uint16_t ch);

// Emit one complete frame: BREAK, MAB, null start code, then DMX_FRAME_SLOTS
// slots. Blocks until the frame has fully left the UART so that the next call's
// baud-rate change can never corrupt bytes still in flight.
void dmxUpdate();
