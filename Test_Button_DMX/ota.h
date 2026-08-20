#pragma once
#include <Arduino.h>
#include <WebServer.h>

// Over-the-air firmware update over the device's own WiFi AP.
//
// USB flashing writes 1.1 MB one block at a time through a USB-Serial/JTAG
// controller that wedges under load — measured at ~2% efficiency (8.8 s of
// data inside a 461 s flash). The partition table already carries two 3264 KB
// OTA slots plus otadata, and the device already runs a web server, so pushing
// the same binary over WiFi takes seconds and touches none of that hardware.
//
// OTA is the primary upload path; USB remains the recovery path for a device
// that is bricked or has no working WiFi. See docs/spec-ota-update.md.
//
// SAFETY: an upload stalls the main loop, so DMX frames stop for its duration
// and every fixture latches its last commanded value. A valve open at that
// moment would stay open with nothing left running to close it. otaRegister()
// therefore refuses to start unless the rig is provably idle, and drives every
// valve to 0 on the wire before accepting a single byte.
void otaRegister(WebServer& server);

// True while an upload is in flight — used by /api/state.
bool otaInProgress();

// Last upload's error text, empty if none. Survives the response so the UI can
// show why a rejected upload was rejected.
const char* otaLastError();

// --- Shared rig-safety helpers -------------------------------------------
// Both live here because OTA defined them first and documents why they exist;
// DMX quiet mode is the second caller. Any operation that stops DMX frames
// reaching the bus needs both, in this order.

// True when nothing could have a valve open, or open one while frames are
// stopped: FSM IDLE, no purge, no morse, audio disarmed. `why` gets a
// human-readable reason on failure, for the API response.
bool rigSafeToStall(String& why);

// Drive every channel to 0 and push real frames until it has reached the wire.
// Zeroing the shadow buffer is not enough — fixtures latch their last
// COMMANDED value, so the zeros have to actually be transmitted.
void rigForceEverythingClosed();
