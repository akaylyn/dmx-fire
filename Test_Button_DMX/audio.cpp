#include <Arduino.h>
#include <WiFiUdp.h>
#include "audio.h"
#include "button_fsm.h"
#include "dmx.h"
#include "log.h"

AudioConfig audioConfig;

// Polled WiFiUDP, NOT AsyncUDP. AsyncUDP callbacks run on the LwIP task, and the FSM
// injection flags are non-atomic test-then-clear on volatile bools — a press landing
// between the test and the clear would vanish. Polling from loop() keeps every writer
// on the single thread the rest of this firmware already assumes.
static WiFiUDP g_udp;

static AudioFeatures g_features;

// --- link state ---
static bool      g_armed      = false;   // RAM only, false every boot
static uint32_t  g_lastRxMs   = 0;
static bool      g_everRx     = false;
static IPAddress g_peer;
static bool      g_havePeer   = false;
static uint16_t  g_session    = 0;
static uint16_t  g_seq        = 0;
static bool      g_haveSeq    = false;

// --- counters, all surfaced in /api/state ---
static uint32_t g_packets    = 0;
static uint32_t g_bad        = 0;
static uint32_t g_gaps       = 0;
static uint32_t g_floodTicks = 0;
static uint16_t g_pps        = 0;
static uint16_t g_ppsCount   = 0;
static uint32_t g_ppsWindow  = 0;

// --- edge latches: OR-accumulated across every packet since the last consume ---
// 40 Hz into a 20 Hz consumer means beats are EDGE EVENTS, not samples. Taking only
// the newest packet would silently drop about half of them.
static uint8_t g_latchFlags   = 0;
static uint8_t g_latchBeatStr = 0;

// --- beat grid, anchored to the acoustic instant ---
static uint32_t g_beatAnchorMs   = 0;
static uint32_t g_prevAnchorMs   = 0;
static uint16_t g_beatIntervalMs = 0;
static uint8_t  g_stableBeats    = 0;  // consecutive beats with a consistent interval
static uint32_t g_predictedBeatMs = 0; // the beat a predicted shot already fired for
// Strength of the last actual BEAT, which is not the same as g_features.beatStrength:
// that tracks the newest packet, and non-beat packets carry 0. The predictor fires
// BETWEEN beats, so gating it on the newest packet would see 0 almost every time.
static uint8_t  g_lastBeatStrength = 0;

static void audioResetLink() {
  g_features = AudioFeatures{};
  g_latchFlags     = 0;
  g_latchBeatStr   = 0;
  g_beatAnchorMs   = 0;
  g_prevAnchorMs   = 0;
  g_beatIntervalMs = 0;
  g_stableBeats    = 0;
  g_lastBeatStrength = 0;
  g_havePeer       = false;
  g_haveSeq        = false;
}

void audioSetup() {
  // Code defaults; storageLoad() runs later in setup() and wins where NVS has a value.
  audioConfig.shotMs     = 150;
  audioConfig.minGapMs   = 200;
  audioConfig.dutyPct    = 40;
  audioConfig.dutyWinMs  = 10000;
  audioConfig.maxOpenMs  = 1000;
  audioConfig.leadMs     = 120;   // MEASURE this per rig; see the spec's commissioning note
  audioConfig.staleMs    = 500;   // not 250 — SoftAP UDP bursts past 250 ms routinely
  audioConfig.bassOn     = 170;
  audioConfig.bassOff    = 140;
  audioConfig.beatMin    = 90;
  audioConfig.dropMin    = 200;
  audioConfig.dropGapMs  = 3000;
  audioConfig.dropShotMs = 400;
  audioConfig.lightMode  = 1;
  audioConfig.lightDepth = 150;

  audioResetLink();
}

void audioNetBegin() {
  if (g_udp.begin(AUDIO_UDP_PORT)) {
    LOG_I("[AUDIO] listening on UDP %u", AUDIO_UDP_PORT);
  } else {
    LOG_E("[AUDIO] failed to bind UDP %u", AUDIO_UDP_PORT);
  }
}

// Update the beat grid from a packet that carried AUDIO_FLAG_BEAT.
//
// The anchor is the ACOUSTIC instant, not the arrival time: SoftAP UDP clusters and
// bursts, so arrival is a poor proxy. sinceBeatMs carries the Echo's analysis and send
// latency back out of the timestamp.
static void audioNoteBeat(const AudioPacket& p, uint32_t rxMs) {
  g_lastBeatStrength = p.beatStrength;
  uint16_t lat = p.sinceBeatMs;
  if (lat > AUDIO_SINCE_BEAT_CLAMP_MS) lat = AUDIO_SINCE_BEAT_CLAMP_MS;

  g_prevAnchorMs = g_beatAnchorMs;
  g_beatAnchorMs = rxMs - lat;

  if (g_prevAnchorMs) {
    uint32_t iv = g_beatAnchorMs - g_prevAnchorMs;
    if (iv >= 150 && iv <= 2000) {  // 30..400 BPM; anything else is not a tempo
      uint16_t prev = g_beatIntervalMs;
      g_beatIntervalMs = (uint16_t)iv;
      // "Stable" = within 15% of the previous interval. Four in a row before the
      // predictor is allowed to run: a wrong prediction fires propane into silence.
      if (prev && (uint16_t)abs((int)iv - (int)prev) <= prev / 6) {
        if (g_stableBeats < 255) g_stableBeats++;
      } else {
        g_stableBeats = 0;
      }
    } else {
      g_beatIntervalMs = 0;
      g_stableBeats    = 0;
    }
  }
}

static void audioAgeLink(uint32_t now) {
  if (!g_everRx) return;

  if (now - g_lastRxMs > audioConfig.staleMs) {
    if (g_features.level || g_features.bass || g_beatAnchorMs) {
      LOG_W("[AUDIO] link stale (%lu ms) — features zeroed",
            (unsigned long)(now - g_lastRxMs));
      audioResetLink();
    }
  }

  // packets/sec over a rolling 1 s window, for the Audio tab's link readout
  if (now - g_ppsWindow >= 1000) {
    g_pps       = g_ppsCount;
    g_ppsCount  = 0;
    g_ppsWindow = now;
  }
}

void audioTick() {
  uint32_t now     = millis();
  uint8_t  drained = 0;
  int      len;

  while (drained < AUDIO_MAX_DRAIN && (len = g_udp.parsePacket()) > 0) {
    drained++;
    IPAddress src = g_udp.remoteIP();

    if (len != AUDIO_PKT_SIZE) { g_udp.flush(); g_bad++; continue; }

    AudioPacket p;
    if (g_udp.read((uint8_t*)&p, sizeof(p)) != (int)sizeof(p)) { g_bad++; continue; }

    if (memcmp(p.magic, "DFAU", 4) != 0)   { g_bad++; continue; }
    if (p.version != AUDIO_PKT_VERSION)    { g_bad++; continue; }

    // Source filter. In AP+STA mode (secrets.h sets a station SSID) this port is also
    // reachable from the house LAN, so binding alone is not a filter.
    if (!(src[0] == 192 && src[1] == 168 && src[2] == 4)) { g_bad++; continue; }

    // Peer lock: the first sender owns the link while it stays fresh. Releases on
    // staleness, so an Echo reboot self-heals after staleMs.
    bool fresh = g_everRx && (now - g_lastRxMs <= audioConfig.staleMs);
    if (g_havePeer && fresh && src != g_peer) { g_bad++; continue; }

    if (g_haveSeq && p.session == g_session) {
      int16_t delta = (int16_t)(p.seq - g_seq);
      if (delta <= 0) { g_bad++; continue; }   // duplicate or replay
      g_gaps += (uint32_t)(delta - 1);
    } else if (p.session != g_session) {
      // New Echo boot: reset sequence tracking rather than counting one huge gap.
      LOG_I("[AUDIO] session %u (was %u) from %s",
            p.session, g_session, src.toString().c_str());
      g_session = p.session;
    }
    g_seq     = p.seq;
    g_haveSeq = true;

    g_peer     = src;
    g_havePeer = true;
    g_lastRxMs = now;
    g_everRx   = true;
    g_packets++;
    g_ppsCount++;

    // Bands come from the NEWEST packet; edges are OR-latched across all of them.
    g_features.bass           = p.bass;
    g_features.mid            = p.mid;
    g_features.treble         = p.treble;
    g_features.level          = p.level;
    g_features.bpm            = p.bpm;
    g_features.silence        = (p.flags & AUDIO_FLAG_SILENCE) != 0;
    g_features.clip           = (p.flags & AUDIO_FLAG_CLIP) != 0;

    // Features track the newest sample; the latches hold pending EDGES until a
    // consumer drains them. Keeping them separate matters: if the latched maximum
    // leaked into the snapshot it would stick at its high-water mark forever,
    // because nothing clears a latch except audioConsume*().
    g_features.beatStrength = p.beatStrength;

    g_latchFlags |= p.flags;
    if (p.beatStrength > g_latchBeatStr) g_latchBeatStr = p.beatStrength;

    if (p.flags & AUDIO_FLAG_BEAT) audioNoteBeat(p, now);

    g_features.beatIntervalMs = g_beatIntervalMs;
  }

  if (drained >= AUDIO_MAX_DRAIN) {
    // Socket still had more. Bounded on purpose — the DMX frame gate must not be
    // starved by a flood. Counted so the field can see it happening.
    g_floodTicks++;
  }

  audioAgeLink(now);
}

bool audioFresh() {
  if (!g_everRx) return false;
  return (millis() - g_lastRxMs) <= audioConfig.staleMs;
}

bool audioArmed() { return g_armed; }

void audioArm() {
  if (!g_armed) LOG_I("[AUDIO] ARMED");
  g_armed = true;
  // Arming must not inherit a beat grid or a pending edge from before it was armed.
  // staleMs is 500 ms, so a grid locked moments earlier — under a different mode,
  // different thresholds, or a different track — is still "confident" at the instant
  // the operator arms, and the predictor would fire on it before a single new packet
  // had been evaluated. Make it re-lock from scratch.
  g_beatAnchorMs    = 0;
  g_prevAnchorMs    = 0;
  g_beatIntervalMs  = 0;
  g_stableBeats     = 0;
  g_predictedBeatMs  = 0;
  g_lastBeatStrength = 0;
  g_latchFlags       = 0;
  g_latchBeatStr     = 0;
}

void audioDisarm() {
  if (g_armed) LOG_I("[AUDIO] disarmed");
  g_armed = false;
  // Close anything audio has open right now. audioFireTick() would also catch this on
  // its next pass, but disarm is an operator safety action — it should not wait a
  // loop iteration to take effect.
  audioAbortShot();
}

const AudioFeatures& audioSnapshot() { return g_features; }

// Fast-attack, slow-decay envelope for the light path.
//
// The decay floor is AUDIO_LIGHT_DECAY_MS and is NOT configurable: a hard on/off gate
// at beat rate is a photosensitivity hazard, which is the same reason the uplight
// deliberately does not strobe with mgOn. Attack is instant so a kick still reads as
// a hit; only the release is slewed.
static uint8_t  g_env       = 0;
static uint32_t g_envLastMs = 0;

uint8_t audioEnvelope() {
  uint32_t now = millis();
  if (!audioFresh()) { g_env = 0; g_envLastMs = now; return 0; }

  uint32_t dt = now - g_envLastMs;
  g_envLastMs = now;

  uint8_t target = g_features.level;
  if (target >= g_env) {
    g_env = target;                       // attack: immediate
  } else if (dt) {
    // Linear release over AUDIO_LIGHT_DECAY_MS from full scale.
    uint32_t drop = (dt * 255u) / AUDIO_LIGHT_DECAY_MS;
    g_env = (drop >= g_env) ? target : (uint8_t)(g_env - drop);
    if (g_env < target) g_env = target;
  }
  return g_env;
}

// Edge consumers — test-and-clear, exactly like buttonConsumePress(). No caller until
// the fire modes land; the latch semantics are fixed now so they cannot drift later.
bool audioConsumeBeat() {
  bool had = (g_latchFlags & AUDIO_FLAG_BEAT) != 0;
  g_latchFlags &= ~AUDIO_FLAG_BEAT;
  return had;
}

bool audioConsumeBigHit() {
  bool had = (g_latchFlags & AUDIO_FLAG_BIGHIT) != 0;
  g_latchFlags &= ~AUDIO_FLAG_BIGHIT;
  return had;
}

uint8_t audioPeakBeatStrength() {
  uint8_t v = g_latchBeatStr;
  g_latchBeatStr = 0;
  return v;
}

// True while a beat landed recently enough to blink an indicator. Derived from the
// grid rather than the latch, so the UI cannot steal an edge the fire path needs.
bool audioBeatRecent(uint16_t withinMs) {
  if (!audioFresh() || !g_beatAnchorMs) return false;
  return (millis() - g_beatAnchorMs) < withinMs;
}

uint32_t audioBeatAnchorMs()   { return g_beatAnchorMs; }
uint16_t audioBeatIntervalMs() { return g_beatIntervalMs; }

bool audioBeatGridConfident() {
  return audioFresh() && g_beatIntervalMs != 0 && g_features.bpm != 0
         && g_stableBeats >= 4;
}

uint32_t audioPackets()    { return g_packets; }
uint32_t audioBadPackets() { return g_bad; }
uint32_t audioSeqGaps()    { return g_gaps; }
uint32_t audioFloodTicks() { return g_floodTicks; }
uint16_t audioPps()        { return g_pps; }

uint32_t audioAgeMs() {
  if (!g_everRx) return UINT32_MAX;
  return millis() - g_lastRxMs;
}

IPAddress audioPeer() { return g_havePeer ? g_peer : IPAddress(0, 0, 0, 0); }

// ===========================================================================
// The limiter — the rate authority for audio-driven fire.
//
// cooldownMs cannot do this job: at 120 BPM a shot is due every 500 ms, and any
// nonzero cooldown silently swallows presses. But simply telling the operator to
// set cooldownMs=0 would leave the PHYSICAL button with no lockout the moment they
// switched back to mode 0. So audio brings its own limiter and never touches the
// operator's stored cooldown.
// ===========================================================================

static int32_t  g_used100   = 0;   // open-ms x100, leaky-bucket accumulator
static uint32_t g_lastAge   = 0;
static uint32_t g_lastShotStartMs = 0;
static uint32_t g_lastShotEndMs   = 0;
static uint32_t g_lastDropMs      = 0;

// Shot ownership. Tracked so the cooldown skip below can be scoped to shots audio
// itself started — see fsmSkipCooldown()'s comment.
static bool     g_shotActive      = false;
static bool     g_lastShotWasAudio = false;
static uint32_t g_shotStartMs     = 0;
static uint16_t g_shotGrantMs     = 0;

static void limiterAge() {
  uint32_t now = millis();
  uint32_t dt  = now - g_lastAge;   // unsigned subtraction — wrap-safe
  g_lastAge = now;
  // Leak at dutyPct: dt ms drains dt * pct hundredths. Long-run duty converges
  // exactly to dutyPct with no ring buffer and no floating point.
  g_used100 -= (int32_t)(dt * audioConfig.dutyPct);
  if (g_used100 < 0) g_used100 = 0;
}

uint16_t audioDutyCapMs() {
  uint32_t cap = (uint32_t)audioConfig.dutyWinMs * audioConfig.dutyPct / 100;
  // A leaky bucket ALONE is not safe: at a legal max config (60% of 60 s) it would
  // permit ~36 s of near-continuous propane before filling. Ceilinged at what one
  // default manual FIREBALL press already delivers.
  if (cap > AUDIO_BURST_CEIL_MS) cap = AUDIO_BURST_CEIL_MS;
  return (uint16_t)cap;
}

uint16_t audioDutyUsedMs() {
  limiterAge();
  return (uint16_t)(g_used100 / 100);
}

// Count real commanded valve-open time. Called ONCE per DMX frame from the frame
// block, which is the only place that knows what actually reached the wire.
void audioNoteFrame(bool anyValveOpen) {
  limiterAge();
  if (anyValveOpen) g_used100 += (int32_t)DMX_FRAME_INTERVAL_MS * 100;
}

uint16_t audioLimiterGrant(uint16_t requestedMs) {
  if (!audioArmed() || !audioFresh() || !audioMode(buttonConfig.mode)) return 0;
  if (audioConfig.dutyPct == 0) return 0;   // 0 = lights only, never fire
  uint32_t now = millis();
  // Rate limit, measured from the previous shot's START.
  if (now - g_lastShotStartMs < audioConfig.minGapMs) return 0;
  // Separate OFF-time floor, measured from the previous shot's END. Without this the
  // two can be satisfied simultaneously and consecutive shots merge into one burn that
  // sails past the burst ceiling — see AUDIO_MIN_OFF_MS.
  if (g_lastShotEndMs && (now - g_lastShotEndMs) < AUDIO_MIN_OFF_MS) return 0;

  limiterAge();
  int32_t remain = ((int32_t)audioDutyCapMs() * 100 - g_used100) / 100;
  if (remain < 0) remain = 0;

  uint32_t grant = requestedMs;
  if (grant > audioConfig.maxOpenMs)            grant = audioConfig.maxOpenMs;
  if (grant > buttonConfig.fireDurationMs)      grant = buttonConfig.fireDurationMs;
  if (grant > (uint32_t)remain)                 grant = (uint32_t)remain;

  // A grant shorter than one frame is a lie: the fsmConsumeFirePending() latch still
  // puts a full frame of gas on the wire, so the budget would under-count.
  if (grant < DMX_FRAME_INTERVAL_MS) return 0;
  return (uint16_t)grant;
}

bool audioShotActive() { return g_shotActive; }

void audioAbortShot() {
  if (!g_shotActive) return;
  buttonInjectRelease();
  g_lastShotEndMs = millis();
  // The release alone is not enough. modeClosesOnRelease() is false for FIREBALL, so
  // if the operator switched to mode 0 mid-burn the FSM would ignore it and hold the
  // valve open for the rest of fireDurationMs. Closing is always safe.
  fsmEndFireNow();
  g_shotActive = false;
}

// ===========================================================================
// Beat prediction
//
// A reactive trigger cannot land on the beat: mic analysis, WiFi, the 20 Hz frame
// gate, the solenoid and gas transit are all additive, typically 100-250 ms, against
// a perceptual sync window of roughly 150 ms at club tempo — and listeners are more
// sensitive to sound-before-light, which is exactly how reacting errs. So schedule
// against the NEXT beat and fire leadMs early.
// ===========================================================================


// Next beat instant in the millis() domain, or 0 if there is no usable grid.
static uint32_t audioNextBeatMs() {
  if (!audioBeatGridConfident()) return 0;
  uint32_t anchor = g_beatAnchorMs;
  uint16_t iv     = g_beatIntervalMs;
  if (!anchor || !iv) return 0;

  uint32_t next = anchor + iv;
  // Never schedule more than one beat ahead: if the grid has drifted far enough that
  // the next beat is already in the past, the schedule is stale, not merely late.
  if ((int32_t)(next - millis()) < -(int32_t)iv) return 0;
  return next;
}

// True on the loop iteration where a predicted shot is due.
static bool audioPredictedDue() {
  uint32_t next = audioNextBeatMs();
  if (!next) return false;
  if (next == g_predictedBeatMs) return false;   // already fired this beat

  uint32_t fireAt = next - audioConfig.leadMs;
  if ((int32_t)(millis() - fireAt) >= 0) {
    g_predictedBeatMs = next;
    return true;
  }
  return false;
}

// ===========================================================================
// Mode 6's pulse gate. Cannot reuse mode 2's `millis() % period` — that is anchored
// to boot, which has no relationship to the music.
// ===========================================================================

bool audioBeatGate(uint16_t onMs) {
  if (!audioFresh() || !g_beatAnchorMs) return false;   // fail closed
  uint16_t iv = g_beatIntervalMs ? g_beatIntervalMs : 500;
  uint32_t since = millis() - g_beatAnchorMs;
  // Grid lost: two intervals with no beat means stop pulsing rather than free-run.
  if (since > (uint32_t)iv * 2 + 250) return false;
  // Lead-compensate the same way the trigger does, so the pulse train sits on the
  // beat rather than behind it.
  uint32_t adj = since + audioConfig.leadMs;
  if (adj >= iv) adj -= iv;
  return adj < onMs;
}

// ===========================================================================
// Per-mode decision. Runs every loop iteration, before the button merge.
// ===========================================================================

// Slow average of `level`, for controller-side transient detection. Implemented here
// as well as honouring the Echo's BIGHIT flag so drop mode is testable before the
// Echo exists, and so a weak onset detector cannot kill the mode.
static uint16_t g_levelAvg = 0;

static void audioUpdateLevelAvg() {
  // Single pole, tau ~2 s at loop rate. Scaled x256 to stay in integers.
  uint16_t lvl = (uint16_t)g_features.level << 8;
  g_levelAvg += (int16_t)((lvl - g_levelAvg) >> 9);
}

static bool audioTransientDetected() {
  uint16_t avg = g_levelAvg >> 8;
  return g_features.level > avg && (g_features.level - avg) >= audioConfig.dropMin;
}

void audioFireTick() {
  uint32_t now = millis();
  audioUpdateLevelAvg();

  // 1. Fail-safe FIRST, unconditionally — this must run even when disarmed, stale or
  //    in the wrong mode, because any of those can become true mid-shot.
  if (g_shotActive) {
    bool expired = (now - g_shotStartMs) >= g_shotGrantMs;
    if (expired || !audioArmed() || !audioFresh() || !audioMode(buttonConfig.mode)) {
      audioAbortShot();   // single close path — see the comment there
    }
  }

  if (!audioArmed() || !audioFresh() || !audioMode(buttonConfig.mode)) {
    // Drain the edge latches so a stale beat cannot fire the instant we re-arm.
    audioConsumeBeat();
    audioConsumeBigHit();
    audioPeakBeatStrength();
    g_predictedBeatMs = 0;
    return;
  }

  if (g_shotActive) return;   // a shot is already running; nothing to decide

  // 2. Return the FSM to IDLE without waiting out a lockout meant for manual fire.
  //    SCOPED to audio-initiated shots: without this check the operator's physical
  //    button would also lose its lockout while the device sat in an audio mode.
  if (g_lastShotWasAudio) fsmSkipCooldown();

  if (fsmState != FSM_IDLE) return;   // someone else owns the valve — do not fight

  // 3. Per-mode decision -> how long we would like the valve open.
  uint16_t want = 0;
  switch (buttonConfig.mode) {
    case 3: {  // beat pop — predicted, with the reactive edge as fallback
      bool beat = audioConsumeBeat();
      uint8_t strength = audioPeakBeatStrength();
      if (audioBeatGridConfident()) {
        // Prediction changes WHEN a shot is requested, never WHETHER it is allowed —
        // so the strength gate applies here too, judged on the most recent beat.
        // Without this, beatMin is silently bypassed the moment a grid locks.
        if (g_lastBeatStrength >= audioConfig.beatMin && audioPredictedDue())
          want = audioConfig.shotMs;
      } else if (beat && strength >= audioConfig.beatMin) {
        want = audioConfig.shotMs;
      }
      break;
    }
    case 4: {  // sustained bass — a level, not an instant, so never predicted
      if (g_features.bass >= audioConfig.bassOn) want = audioConfig.maxOpenMs;
      break;
    }
    case 5: {  // drop only — unpredictable by definition
      bool hit = audioConsumeBigHit();
      if ((hit || audioTransientDetected())
          && (now - g_lastDropMs) >= audioConfig.dropGapMs) {
        want = audioConfig.dropShotMs;
        g_lastDropMs = now;
      }
      break;
    }
    case 6: {  // machine gun on beat — held open; audioBeatGate() pulses the valve
      if (g_features.level >= audioConfig.bassOn && audioBeatGridConfident())
        want = audioConfig.maxOpenMs;
      break;
    }
    default: break;
  }

  if (!want) return;

  uint16_t grant = audioLimiterGrant(want);
  if (!grant) return;

  buttonInjectPress();
  g_shotActive       = true;
  g_lastShotWasAudio = true;
  g_shotStartMs      = now;
  g_lastShotStartMs  = now;
  g_shotGrantMs      = grant;
}

// Mode 4 needs to close on the hysteresis threshold rather than only on grant expiry.
// Called from the same place as audioFireTick(); kept separate so the release path
// stays obvious.
void audioSustainTick() {
  if (!g_shotActive || buttonConfig.mode != 4) return;
  if (g_features.bass < audioConfig.bassOff) {
    audioAbortShot();
  }
}
