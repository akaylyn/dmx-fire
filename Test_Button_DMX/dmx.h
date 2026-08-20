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

// Slots transmitted per frame. Back at 64 (== the addressed universe) after the
// 128-slot padding experiment regressed the accumulator light — see notes.md
// "START HERE". The padding idea (a console sends 512) is kept as a one-line
// experiment: raise this and dmxLastFrame grows with it; slots beyond
// DMX_SHADOW_SIZE stay zero so padding can never drive a fixture. Re-scope the
// bus before trusting any value other than 64.
static const uint16_t DMX_FRAME_SLOTS = 64;

// The frame buffer IS what gets transmitted — there is no second copy to keep in
// sync. Index 0 = DMX channel 1. The first DMX_SHADOW_SIZE entries are live
// channel data; the rest is zero padding.
extern uint8_t dmxLastFrame[DMX_FRAME_SLOTS];

// Frame interval in ms. A 64-slot frame (break + start code + 64 slots) occupies
// ~2.9 ms on the wire, so the line idles for the remainder of each interval.
// Deliberately kept SLOW (20 Hz): field experience is that a slower, unhurried
// transmitter is more reliable on this bus than pushing toward continuous refresh
// — fewer frames to collide with loop jitter, more settle margin for the cheap
// decoders. The trade-off is a longer high-Z idle window between frames (notes.md
// causes #2/#4); revisit only if idle-line noise reappears. DMX fixtures tolerate
// 20 Hz well (spec floor is ~1 Hz).
static const uint8_t DMX_FRAME_INTERVAL_MS = 50;  // 20 Hz — ~47 ms idle per frame

// Frame accounting for troubleshooting. sent = frames that reached the wire;
// skipped = ticks where the previous frame had not finished draining. A rising
// skipped count means the TX path is starving the bus. dmx.cpp always defined
// these and claimed they were "exposed via dmx.h" — they were not, which is why
// nothing ever printed them.
extern uint32_t dmxFramesSent;
extern uint32_t dmxFramesSkipped;

void dmxSetup();

// --- Transmitter quiet mode (bench testing) -------------------------------
// Stops emitting frames entirely so a second controller — a manual console or
// the Enttec — can drive the bus without colliding with this one. DMX has no
// arbitration: two transmitters on one pair is a garbled bus, not a merge.
//
// This is necessarily GLOBAL. A frame carries every slot 1..DMX_FRAME_SLOTS, so
// there is no such thing as going quiet for one tower — the per-tower
// equivalent is TowerConfig.connected, which keeps transmitting that tower's
// channels as zero.
//
// SAFETY: fixtures hold their last commanded value when the signal stops, so a
// valve open at that moment stays open with nothing left to close it. Callers
// MUST drive everything to 0 and flush it to the wire before going quiet —
// rigForceEverythingClosed() in ota.h does exactly this. dmxUpdate() also
// refuses to transmit while quiet, so no other caller can break the silence.
//
// Deliberately RAM-only and never persisted. A saved "stop transmitting" flag
// is the same trap as a saved connected=false: it survives reboots and reads as
// dead hardware. A power cycle always restores normal output. See notes.md
// Session 5.
void dmxSetQuiet(bool quiet);
bool dmxQuiet();

// Write a byte into the frame buffer. 1-indexed; channels outside
// 1..DMX_SHADOW_SIZE are ignored so the padding stays zero.
void dmxShadowWrite(uint8_t value, uint16_t ch);

// True when the previous frame has fully drained from the UART TX buffer, so a
// new break can start without a baud-rate change landing on bytes still enqueued.
// Non-blocking — queries Serial1.availableForWrite() against the idle buffer size
// captured in dmxSetup(). dmxUpdate() consults this before emitting; exposed so
// a caller (or a test) can pace frames without spinning on flush().
bool dmxReadyToSend();

// Emit one complete frame: BREAK, MAB, null start code, then DMX_FRAME_SLOTS
// slots. Skips the frame (returns without touching the wire) if the previous one
// has not finished draining — see dmxReadyToSend(). Otherwise blocks until the
// frame has fully left the UART so that the next call's baud-rate change can
// never corrupt bytes still in flight.
void dmxUpdate();
