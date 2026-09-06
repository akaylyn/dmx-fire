/*
 * @Hardwares: M5AtomS3 Lite + Unit DMX
 * @Platform Version: Arduino M5Stack Board Manager v2.1.3
 * @Dependent Library:
 * M5Unified@^0.2.11: https://github.com/m5stack/M5Unified
 * FastLED@^3.9.10: https://github.com/FastLED/FastLED
 * DMX512 frames are emitted directly on Serial1 by dmx.cpp (see dmx.h for why
 * SparkFunDMX::update() is no longer used).
 */

#include <M5Unified.h>
#include <FastLED.h>
#include "board_config.h"
#include "themes.h"
#include "dmx.h"
#include "towers.h"
#include "confluence.h"
#include "button_fsm.h"
#include "storage.h"
#include "web.h"
#include "audio.h"
#include "log.h"
#include "morse.h"
#include "tests.h"

// Pins and feature flags come from board_config.h so a bench target (e.g.
// CoreS3, used to exercise the upload paths) compiles without a NeoPixel or a
// GPIO39 button it does not have.
#if HAS_PHYSICAL_BUTTON
m5::Button_Class keyButton;
#endif

#if HAS_STATUS_LED
CRGB ATOM_LED[1];
#endif

// Swap the uplight over to the configured fire look. Touches ONLY the uplight
// fields — the accumulator strips (r/g/b) keep running the theme underneath, so
// firing changes what the uplight shows without disturbing the strips.
// White is assigned rather than max()'d: the fire look owns the uplight while a
// valve is open, and the end-cue fade runs afterwards, never at the same time.
static inline void applyFireLook(TowerState& s) {
  s.ur    = buttonConfig.fireUpR;
  s.ug    = buttonConfig.fireUpG;
  s.ub    = buttonConfig.fireUpB;
  s.white = buttonConfig.fireUpW;
}

// Audio-reactive glow, layered UNDER the fire / end-cue / purge chain so every
// existing override still looks exactly as it did.
//
// Composition is max(), additive — never multiplicative. The green/blue/fire gradient
// themes return a fully zeroed TowerState for 3200 ms of every 4000 ms cycle, so a
// multiplicative modulation would be invisible 80% of the time. An additive glow
// lights the blank phase and can never dim the bright phase.
//
// Colour reuses the fire-uplight look, so audio modulation reads as the fire
// breathing before it fires. No new colour config.
static inline void applyAudioLook(TowerState& s, uint8_t index) {
  if (audioConfig.lightMode == 0) return;

  const AudioFeatures& a = audioSnapshot();

  // Envelope is computed in audio.cpp with a hard decay floor; here we only scale it.
  // No hard square gate anywhere — strobing at beat rate is a photosensitivity
  // hazard, the same reason the uplight does not strobe with mgOn.
  uint16_t depth = audioConfig.lightDepth;

  if (audioConfig.lightMode == 2) {
    // Band mode: bass/mid/treble drive the uplight RGB. Strips keep the theme.
    uint8_t r = (uint8_t)((uint16_t)a.bass   * depth / 255);
    uint8_t g = (uint8_t)((uint16_t)a.mid    * depth / 255);
    uint8_t b = (uint8_t)((uint16_t)a.treble * depth / 255);
    if (r > s.ur) s.ur = r;
    if (g > s.ug) s.ug = g;
    if (b > s.ub) s.ub = b;
    return;
  }

  // Pulse mode: the whole uplight breathes with the level envelope, in the fire
  // colour. Strips get a smaller accent so the accumulator body moves too.
  uint16_t env = (uint16_t)audioEnvelope() * depth / 255;
  uint8_t  ur  = (uint8_t)((uint16_t)buttonConfig.fireUpR * env / 255);
  uint8_t  ug  = (uint8_t)((uint16_t)buttonConfig.fireUpG * env / 255);
  uint8_t  ub  = (uint8_t)((uint16_t)buttonConfig.fireUpB * env / 255);
  if (ur > s.ur) s.ur = ur;
  if (ug > s.ug) s.ug = ug;
  if (ub > s.ub) s.ub = ub;

  uint8_t sr = (uint8_t)((uint16_t)ur / 3);
  uint8_t sg = (uint8_t)((uint16_t)ug / 3);
  uint8_t sb = (uint8_t)((uint16_t)ub / 3);
  if (sr > s.r) s.r = sr;
  if (sg > s.g) s.g = sg;
  if (sb > s.b) s.b = sb;
  (void)index;  // per-tower phase offset is a later refinement
}

void setup() {
  delay(500);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Ex_I2C.release();  // Free up PORTA so we can use it for Serial1

  Serial.printf("\nStartup. board=%s\n", BOARD_NAME);

#if HAS_PHYSICAL_BUTTON
  pinMode(BUTTON_PIN, INPUT_PULLUP);
#endif

#if HAS_STATUS_LED
  pinMode(STATUS_LED_PIN, OUTPUT);
  FastLED.addLeds<WS2812, STATUS_LED_PIN, GRB>(ATOM_LED, 1);
  ATOM_LED[0] = CRGB::Red;
  FastLED.setBrightness(255);
  FastLED.show();
#endif

  dmxSetup();
  towerSetup();
  confluenceSetup();
  buttonFsmSetup();
  audioSetup();   // code defaults only; storageLoad() below overrides from NVS
  storageLoad();  // overwrite defaults with persisted config before web UI starts
  webSetup();
  audioNetBegin();  // bind UDP 4210 — needs the SoftAP up, so it follows webSetup()

  runDiagnostics();  // prints pass/fail report to serial; also runs a 500 ms DMX visual test
}

void loop() {
  M5.update();
  webTick();
  // Drain the audio socket in loop context, on the same single thread as every other
  // writer. Placed after webTick() so an arm/disarm POST handled this iteration is
  // already visible, and before the button merge below so a beat received now reaches
  // buttonFsmTick() in this same iteration rather than the next one.
  audioTick();
#if HAS_STATUS_LED
  FastLED.show();
#endif

#if HAS_PHYSICAL_BUTTON
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);
  keyButton.setRawState(millis(), pressed);
#endif

  // No keepalive here: the 50 Hz frame loop below is the single DMX writer. A
  // second writer could call update() while a frame was still in the shift
  // register, and the baud-rate change for the next break would mangle its tail.

  // OR physical events with API-injected events so the FSM is single-sourced.
  // Without a physical button (bench targets) only the API path drives the FSM,
  // so a floating pin can never look like a held button and fire on its own.
  // Audio decides and injects BEFORE the merge below, so a beat received this
  // iteration reaches buttonFsmTick() in this same iteration rather than the next.
  audioFireTick();
  audioSustainTick();

#if HAS_PHYSICAL_BUTTON
  bool btnPressed  = keyButton.wasPressed()  || buttonConsumePress();
  bool btnReleased = keyButton.wasReleased() || buttonConsumeRelease();
  bool btnHeld     = keyButton.isPressed()   || buttonVirtualHeld();
#else
  bool btnPressed  = buttonConsumePress();
  bool btnReleased = buttonConsumeRelease();
  bool btnHeld     = buttonVirtualHeld();
#endif
  if (btnPressed)  LOG_I("[BTN] pressed");
  if (btnReleased) LOG_I("[BTN] released");
  buttonFsmTick(btnPressed, btnReleased, btnHeld);

  // DMX output. Interval lives in dmx.h — a ~3.1 ms frame leaves the rest of the
  // interval idle, so lowering it shortens the idle window on the bus.
  EVERY_N_MILLISECONDS(DMX_FRAME_INTERVAL_MS) {
    // Purge overlay: held-open "empty the accumulator" action, independent of
    // the FSM. Bypasses fireDurationMs and cooldown entirely.
    bool purge = purgeActive();

    // Fire gate for this frame. fsmConsumeFirePending() drains a latch, so it is
    // called EXACTLY ONCE per frame, before the tower loop — that way all five
    // valves (four towers + Confluence) agree on the same frame, and a
    // FIRE_ACTIVE window shorter than one frame still reaches the wire.
    //
    // Drained into its own variable rather than written inline as
    // `(fsmState == FSM_FIRE_ACTIVE) || fsmConsumeFirePending()`: `||`
    // short-circuits, so during a normal burn the latch would never be consumed
    // and would then fire a spurious extra frame of valve-open after the FSM had
    // already left FIRE_ACTIVE.
    bool firePending = fsmConsumeFirePending();
    bool firing = (fsmState == FSM_FIRE_ACTIVE) || firePending;

    // MACHINE_GUN pulses every valve in lockstep — towers and Confluence alike.
    // Off-time is one DMX frame, the shortest gap this bus can express.
    bool mgOn = true;
    if (buttonConfig.mode == 2) {
      uint32_t period = (uint32_t)buttonConfig.machineGunBurstMs + DMX_FRAME_INTERVAL_MS;
      mgOn = (millis() % period) < (uint32_t)buttonConfig.machineGunBurstMs;
    } else if (buttonConfig.mode == 6) {
      // Beat-anchored, not boot-anchored. The mode-2 expression above is a free
      // running millis() % period with no relationship to the music, so it cannot
      // be reused here. Fails closed when the beat grid is lost.
      mgOn = audioBeatGate(buttonConfig.machineGunBurstMs);
    }

    for (uint8_t i = 0; i < NUM_TOWERS; i++) {
      if (!towerConfigs[i].connected) {
        // Skipping the block used to mean skipping its VALVE too, and a channel
        // that stops being written keeps its last byte. Un-ticking Connected
        // during a burn therefore left that tower's solenoid latched open with
        // nothing left to close it. Close it explicitly before skipping.
        dmxValveWrite(towerValveChannel(i), false);
        continue;
      }

      // Theme renderer owns the per-frame colour + uplight white in every state,
      // for the strips (r/g/b) and the uplight (ur/ug/ub) alike. The fire look
      // and the end-cue white flash are overlaid on top: the fire look moves only
      // the uplight, so the strips keep running the theme underneath, and the
      // white flash still never opens a valve.
      TowerState state = themeRender(towerConfigs[i].themeName, i, millis(),
                                     towerConfigs[i].bright, towerConfigs[i].speed);

      // Audio glow sits UNDER the fire / end-cue / purge chain below, so those
      // overrides still look exactly as they always did. Gated on freshness alone:
      // lights react whether or not fire is armed.
      if (audioFresh()) applyAudioLook(state, i);

      if (firing) {
        // Open the per-tower propane valve and light the uplight to match. The
        // uplight stays lit for the whole burn even in MACHINE_GUN mode — only
        // the valve pulses, because strobing the uplight at the burst rate reads
        // as a fault and is a photosensitivity hazard.
        state.fireOpen = mgOn && towerConfigs[i].fireEnabled;
        applyFireLook(state);
      } else if (fsmState == FSM_END_CUE && buttonConfig.endCueMs > 0) {
        // White flash fade on the uplight white channel (not the valve), scaled
        // to the configured end-cue length.
        uint32_t elapsed = fsmElapsedMs();
        uint8_t  fade = (elapsed < (uint32_t)buttonConfig.endCueMs)
                        ? (uint8_t)(255 - elapsed * 255 / buttonConfig.endCueMs)
                        : 0;
        if (fade > state.white) state.white = fade;
      }
      // IDLE / COOLDOWN: theme only.

      // Purge wins over the FSM: hold this tower's accumulator valve fully open
      // and light its uplight, so an open valve is never visually silent.
      if (purge) {
        state.fireOpen = towerConfigs[i].fireEnabled;
        applyFireLook(state);
      }

      towerWrite(i, state);
    }

    if (confluenceConfig.connected) {
      // Open or shut, never a level — the solenoid has no third position.
      // fireEnabled gates every source alike, so switching it off isolates the
      // central valve from the button, purge and Morse in one place.
      bool cfOpen = false;
      if (purge) {
        cfOpen = confluenceConfig.fireEnabled;
      } else if (morseActive()) {
        // morseTick() must run even when fire is disabled: it advances playback,
        // so short-circuiting it would freeze the message at its first unit.
        bool unitOn = morseTick();
        cfOpen = unitOn && confluenceConfig.fireEnabled;
      } else if (firing) {
        // Same mgOn gate as the tower valves above, so all five fire together.
        cfOpen = mgOn && confluenceConfig.fireEnabled;
      }
      confluenceWrite(cfOpen);
    } else {
      // Same latch hazard as a disconnected tower: an unwritten CH1 holds its
      // last byte, so disconnecting mid-burn would strand the valve open.
      confluenceWrite(false);
    }

    // Log the VALVE channels on every state or purge change.
    //
    // This used to print slots 1-5 with RGB labels left over from an older channel
    // map, which made it blind to the four tower valves (8/23/38/53) and to purge
    // entirely — the reason field logs looked silent about the towers while
    // solenoids were misbehaving (notes.md). Purge is included in the trigger
    // because it bypasses the FSM, so a purge would otherwise never log a line.
    static FsmState lastDmxLogState = FSM_IDLE;
    static bool     lastPurgeLogged = false;
    if (fsmState != lastDmxLogState || purge != lastPurgeLogged) {
      lastDmxLogState = fsmState;
      lastPurgeLogged = purge;
      LOG_I("[DMX] state=%s%s  valves CH1=%u CH8=%u CH23=%u CH38=%u CH53=%u  "
            "frames sent=%lu skipped=%lu",
            fsmStateName(fsmState), purge ? " PURGE" : "",
            dmxLastFrame[0], dmxLastFrame[7], dmxLastFrame[22],
            dmxLastFrame[37], dmxLastFrame[52],
            (unsigned long)dmxFramesSent, (unsigned long)dmxFramesSkipped);
    }

    // Charge the audio duty budget with what actually reached the wire this frame.
    // A fixture holds its last commanded byte until the next frame, so an open frame
    // IS 50 ms of gas. Counted here because this is the only place that knows.
    //
    // Every source counts — button, Test Fire, morse, purge — because propane is
    // physical, not per-source. The morse term matters: morseTick() drives confluence
    // CH1 directly and is not gated on `firing`, so leaving it out would let a long
    // message burn uncounted.
    audioNoteFrame(purge || (firing && mgOn)
                   || (confluenceConfig.connected && morseActive()));

    dmxUpdate();
  }

  // Onboard LED reflects FSM state
#if HAS_STATUS_LED
  EVERY_N_MILLISECONDS(20) {
    static uint8_t idleHue = 0;
    switch (fsmState) {
      case FSM_FIRE_ACTIVE: ATOM_LED[0] = CRGB::Red;                    break;
      case FSM_END_CUE:     ATOM_LED[0] = CRGB::White;                  break;
      case FSM_COOLDOWN:    ATOM_LED[0] = CRGB(255, 100, 0);            break;
      default:              ATOM_LED[0] = CHSV(idleHue++, 255, 255);    break;
    }
  }
#endif

  if (M5.BtnA.wasPressed()) {
    LOG_I("[BTN] Atom pressed — running diagnostics");
    runDiagnostics();
  }

  // Heartbeat — current state every 5 s so the log is never silent
  EVERY_N_MILLISECONDS(5000) {
    const char* states[] = {"IDLE", "FIRE_ACTIVE", "END_CUE", "COOLDOWN"};
    LOG_I("[STATUS] fsm=%-11s  uptime=%lu s", states[fsmState], millis() / 1000);
  }
}
