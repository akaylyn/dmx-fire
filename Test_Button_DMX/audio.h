#pragma once
#include <Arduino.h>
#include <IPAddress.h>

// Audio-reactive input: an external Atom EchoS3R analyses its own microphone and
// streams feature packets here over UDP. See docs/spec-audio-reactive.md.
//
// DEPENDENCY DIRECTION IS ONE-WAY. audio.cpp may include button_fsm.h; button_fsm.cpp
// must never include this header. The main loop orchestrates, as it does today.
//
// THIS MODULE NEVER WRITES DMX. It may not call towerWrite(), confluenceWrite() or
// dmxUpdate() — the 20 Hz frame block in the .ino is the single writer, and a second
// one would mangle a frame mid-shift-register. Set values; let the frame block read.

// ---------------------------------------------------------------------------
// Wire format — mirrors tools/audio-sim/audio_packet.py exactly. Little-endian on
// both ends and every field naturally aligned, so packed costs nothing and a memcpy
// is correct without per-field swapping.
// ---------------------------------------------------------------------------

static const uint16_t AUDIO_UDP_PORT    = 4210;  // DNS owns 53, DHCP 67, HTTP 80
static const uint8_t  AUDIO_PKT_VERSION = 1;
static const uint8_t  AUDIO_PKT_SIZE    = 24;

// flags bitfield
static const uint8_t AUDIO_FLAG_BEAT     = 1 << 0;
static const uint8_t AUDIO_FLAG_BIGHIT   = 1 << 1;
static const uint8_t AUDIO_FLAG_CLIP     = 1 << 2;
static const uint8_t AUDIO_FLAG_SILENCE  = 1 << 3;
static const uint8_t AUDIO_FLAG_MICFAULT = 1 << 4;

struct __attribute__((packed)) AudioPacket {
  char     magic[4];      // "DFAU"
  uint8_t  version;
  uint8_t  flags;
  uint16_t session;       // random per Echo boot; resets seq tracking
  uint16_t seq;
  uint8_t  bass, mid, treble, level, beatStrength, bpm;
  uint16_t sinceBeatMs;   // acoustic beat instant -> send time
  uint16_t frameMs;       // Echo's analysis hop, nominal 25
  uint32_t reserved;
};
static_assert(sizeof(AudioPacket) == AUDIO_PKT_SIZE,
              "AudioPacket must be 24 bytes on the wire");

// Drained per loop() iteration. Bounds the work a flood can force so the DMX frame
// gate is never starved; the excess is counted, not silently dropped.
static const uint8_t AUDIO_MAX_DRAIN = 8;

// A bogus sinceBeatMs would park the beat anchor arbitrarily far in the past and make
// the mode-6 gate unpredictable. This clamp is load-bearing, not defensive padding.
static const uint16_t AUDIO_SINCE_BEAT_CLAMP_MS = 200;

// Minimum OFF time between consecutive audio shots.
//
// Without this the burst ceiling does not hold. minGapMs is measured from shot START,
// so with a 3000 ms shot and a 100 ms gap the requirement is already met the instant
// the shot ends — mode 4 re-requests immediately, the valve reopens inside one DMX
// frame, and consecutive shots MERGE into a single continuous burn (measured: 4.46 s
// against a 3.0 s ceiling). Two frames guarantees the bus actually shows the valve
// closed, so a run can never exceed one grant.
static const uint16_t AUDIO_MIN_OFF_MS = 100;

// Hard ceiling on the duty bucket, independent of configuration. 3000 ms is exactly
// what one default manual FIREBALL press already delivers (fireDurationMs default
// 3000): nothing audio-driven may put more propane out in a burst than the operator's
// own button already can. A leaky bucket ALONE is not safe — see the spec.
static const uint16_t AUDIO_BURST_CEIL_MS = 3000;

// Minimum decay for any audio light term. Non-configurable on purpose: a hard square
// gate at beat rate is a photosensitivity hazard, the same reason the uplight does not
// strobe with mgOn.
static const uint16_t AUDIO_LIGHT_DECAY_MS = 180;

// Modes 3..6 live in buttonConfig.mode, which stays a plain uint8_t to match the
// existing style. 0=FIREBALL 1=PARTY 2=MACHINE_GUN 3=beat pop 4=sustained bass
// 5=drop only 6=machine gun on beat.
static const uint8_t AUDIO_MODE_MIN = 3;
static const uint8_t AUDIO_MODE_MAX = 6;
static inline bool audioMode(uint8_t m) {
  return m >= AUDIO_MODE_MIN && m <= AUDIO_MODE_MAX;
}

// ---------------------------------------------------------------------------
// Config — persisted (see storage.cpp). Clamp bounds live here so the spec, the web
// slider ranges and handleSet() cannot drift apart.
// ---------------------------------------------------------------------------

struct AudioConfig {
  uint16_t shotMs;       // beat-pop shot length
  uint16_t minGapMs;     // minimum interval between audio triggers
  uint8_t  dutyPct;      // rolling duty ceiling; 0 = lights only, never fire
  uint16_t dutyWinMs;    // rolling window
  uint16_t maxOpenMs;    // longest single continuous open
  uint16_t leadMs;       // fire this far AHEAD of a predicted beat
  uint16_t staleMs;      // no packet for this long => not fresh
  uint8_t  bassOn;       // sustained-bass open threshold
  uint8_t  bassOff;      // close threshold; always held below bassOn
  uint8_t  beatMin;      // minimum beatStrength to pop
  uint8_t  dropMin;      // transient threshold for drop mode
  uint16_t dropGapMs;    // minimum gap between drops
  uint16_t dropShotMs;   // drop shot length
  uint8_t  lightMode;    // 0 off, 1 pulse, 2 band
  uint8_t  lightDepth;   // modulation depth
};
extern AudioConfig audioConfig;

#define AUD_SHOT_MIN      50
#define AUD_SHOT_MAX      2000
#define AUD_GAP_MIN       100
#define AUD_GAP_MAX       5000
#define AUD_DUTY_MAX      50
#define AUD_WIN_MIN       2000
#define AUD_WIN_MAX       60000
#define AUD_MAXOPEN_MIN   50
#define AUD_MAXOPEN_MAX   3000
#define AUD_LEAD_MAX      500
#define AUD_STALE_MIN     100
#define AUD_STALE_MAX     2000
#define AUD_DROPGAP_MIN   500
#define AUD_DROPGAP_MAX   30000
#define AUD_DROPSHOT_MIN  50
#define AUD_DROPSHOT_MAX  2000

// ---------------------------------------------------------------------------
// Features — a snapshot that stays stable for a whole loop() iteration.
// ---------------------------------------------------------------------------

struct AudioFeatures {
  uint8_t  bass, mid, treble, level, beatStrength, bpm;
  bool     silence, clip;
  uint16_t beatIntervalMs;  // 0 until two beats have been seen
};

// Split in two because the ordering constraints pull opposite ways: the defaults must
// land BEFORE storageLoad() so NVS can override them, but the socket cannot bind until
// webSetup() has brought the SoftAP up.
void audioSetup();     // code defaults only; call before storageLoad()
void audioNetBegin();  // bind the UDP socket; call after webSetup()
void audioTick();      // drain the socket (bounded) and age staleness. Once per loop().

// Decide and inject. Call once per loop(), after audioTick() and BEFORE the button
// merge, so a beat received this iteration reaches buttonFsmTick() in the same one.
void audioFireTick();
void audioSustainTick();  // mode 4's hysteresis release; call right after the above

// --- limiter ---
// audioNoteFrame() must be called EXACTLY ONCE per DMX frame, from the frame block,
// with whether any valve was actually commanded open. It counts real wire time from
// every source — button, Test Fire, morse, purge — because propane is physical.
void     audioNoteFrame(bool anyValveOpen);
uint16_t audioLimiterGrant(uint16_t requestedMs);  // 0 = refuse
uint16_t audioDutyUsedMs();
uint16_t audioDutyCapMs();
bool     audioShotActive();
void     audioAbortShot();  // kill the shot, leave armed alone

// Mode 6's pulse gate — beat-anchored and lead-compensated, fails closed when the
// grid is lost. Replaces mode 2's boot-anchored millis() % period, which has no
// relationship to the music.
bool audioBeatGate(uint16_t onMs);

// --- freshness and arming ---
// armed is RAM-only and false on every boot; it is never persisted. With no arm
// timeout, STALENESS is the primary dead-man: no fresh packets means no fire.
bool audioFresh();
bool audioArmed();
void audioArm();
void audioDisarm();

const AudioFeatures& audioSnapshot();

// Fast-attack, slow-decay level envelope for the light path. The decay floor is
// AUDIO_LIGHT_DECAY_MS and is deliberately not configurable.
uint8_t audioEnvelope();

// Edge consumers — test-and-clear. Beats are edge events at 40 Hz feeding a 20 Hz
// consumer, so they are latched on arrival and drained here rather than sampled.
// Call each at most once per loop() iteration: they clear state.
bool    audioConsumeBeat();
bool    audioConsumeBigHit();
uint8_t audioPeakBeatStrength();     // max since the last call, then resets

// For UI indicators only. Reads the grid, never the latch, so the display cannot
// steal an edge the fire path is waiting on.
bool audioBeatRecent(uint16_t withinMs);

// Beat grid, anchored to the acoustic instant (rx time minus sinceBeatMs) rather than
// to packet arrival. Returns 0 when no usable grid exists.
uint32_t audioBeatAnchorMs();
uint16_t audioBeatIntervalMs();
bool     audioBeatGridConfident();  // stable interval over >= 4 beats, bpm != 0

// --- diagnostics, surfaced in /api/state so the field can see why it is not working ---
uint32_t  audioPackets();
uint32_t  audioBadPackets();
uint32_t  audioSeqGaps();
uint32_t  audioFloodTicks();
uint16_t  audioPps();
uint32_t  audioAgeMs();     // ms since the last accepted packet; UINT32_MAX if none
IPAddress audioPeer();
