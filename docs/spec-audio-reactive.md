# Spec: Audio-reactive fire and lights

## Context

The rig responds to one physical button. This adds a second input: **music**.

An M5Stack Atom EchoS3R joins the controller's SoftAP, analyses its own microphone,
and streams a small UDP feature packet at 40 Hz. The controller uses it two ways —
lights track the audio continuously, and propane fires off the bass in four new
button modes.

The Echo stays a **separate device**, not a peripheral. Its microphone is wired to
its own ESP32 over internal I2S and is not brought out; Grove carries two signals
where I2S needs four. Keeping it separate also solves the placement problem: the
controller must sit at the valve manifold, where a microphone hears propane roar and
blowers rather than music.

**The safety problem this feature creates, and must solve.** Audio-driven fire needs
`cooldownMs = 0` to sync to a beat at all, and `cooldownMs` is the only rate lockout
in the firmware. Without a replacement, propane duty would be bounded by nothing but
`fireDurationMs` and the ~100 ms bus floor — roughly ten shots per second, sustained,
indefinitely. **The audio path therefore carries its own rate authority**, which is
the largest part of this spec.

Related: [spec-rapid-retrigger.md](spec-rapid-retrigger.md) (why the bus is the floor),
[spec-machine-gun.md](spec-machine-gun.md) (the pulse gate this extends),
[spec-purge-accumulator.md](spec-purge-accumulator.md),
[spec-dmx-transmit.md](spec-dmx-transmit.md).

---

## ⚠️ A reactive trigger cannot land on the beat

Every stage between a kick drum and a visible flame is additive:

| Stage | Latency |
|---|---|
| Mic → onset detected | 25–50 ms |
| Echo → controller over WiFi | 0.5–2 ms typical, bursts to 460 ms |
| Loop → next DMX frame (20 Hz gate) | 0–50 ms |
| Frame transmit + decoder | 3–10 ms |
| Solenoid electrical + mechanical | 10–100 ms |
| Gas transit + ignition | rig-specific, **measure it** |

Typical total is **100–250 ms**. The perceptual window for apparent audiovisual
synchrony is roughly 200 ms at 1 Hz and narrows to ~100 ms at 4 Hz — about 150 ms at
club tempo. The asymmetry runs against us: listeners are more sensitive to audio
leading visual than the reverse, which is exactly how a reactive trigger errs.

So modes that ride the beat grid **predict** it. The packet already carries `bpm` and
`sinceBeatMs`, and the receiver derives a beat interval, so the next beat is:

```cpp
uint32_t nextBeatMs = g_beatAnchorMs + g_beatIntervalMs;
uint32_t fireAtMs   = nextBeatMs - audioConfig.leadMs;
```

> ⚠️ **Prediction changes when a shot is requested, never whether it is allowed.**
> A predicted trigger passes every limiter gate exactly like a reactive one.

Prediction degrades detection accuracy, so it engages **only when the tempo estimate
is confident** — a stable interval across at least four beats and a nonzero `bpm`.
Otherwise the reactive `BEAT` edge is used. The schedule never runs more than one
beat ahead and is dropped the moment the grid goes stale.

**Lights stay reactive.** They carry no mechanical or combustion delay, so their
chain is roughly 50–100 ms. Applying `leadMs` to them would make them early. Only the
fire path compensates.

---

## Wire format

**Port 4210/UDP.** DNS occupies 53 on the AP, DHCP 67, HTTP 80; lwIP ephemerals start
at 49152.

```cpp
struct __attribute__((packed)) AudioPacket {
  char     magic[4];      // "DFAU"
  uint8_t  version;       // 1
  uint8_t  flags;         // b0 BEAT, b1 BIGHIT, b2 CLIP, b3 SILENCE, b4 MICFAULT
  uint16_t session;       // random per Echo boot; resets seq tracking
  uint16_t seq;           // +1 per packet, wraps
  uint8_t  bass, mid, treble, level, beatStrength, bpm;
  uint16_t sinceBeatMs;   // acoustic beat instant → send time
  uint16_t frameMs;       // analysis hop, nominal 25
  uint32_t reserved;
};
static_assert(sizeof(AudioPacket) == 24, "AudioPacket must be 24 bytes on the wire");
```

Little-endian both ends, every field naturally aligned, so a `memcpy` into the struct
is correct without per-field swapping.

**No CRC and no HMAC.** lwIP checksums UDP already, and the AP is WPA2 while
`/api/button/press` is unauthenticated — a shared secret here would be theatre. The
receiver enforces what is cheap and real: exact 24-byte length, magic, version, source
in `192.168.4.0/24`, monotonic `seq` within a `session`, and a first-sender peer lock.

**`sinceBeatMs` is why the packet is not smaller.** It carries the Echo's analysis and
send latency so the beat grid anchors to the acoustic instant rather than to packet
arrival. It is clamped to 200 ms — a bogus value would otherwise park the anchor far
in the past and make the mode-6 gate unpredictable. **The clamp is load-bearing.**

> ⚠️ **Beats are edge events, not samples.** 40 Hz into a 20 Hz consumer means the
> drain must OR-latch `flags` and take `max(beatStrength)` across every packet since
> the last read, taking band energies from the newest packet. Using only the last
> packet silently drops about half of all beats.

The 40 Hz rate buys latency, not resolution: a 25 ms hop halves worst-case
beat-to-packet delay, and one lost packet costs 25 ms of staleness instead of 50.

---

## Files

**`audio.h` / `audio.cpp`** (new) — socket, parser, staleness, features, beat grid,
predictor, and the limiter. Dependency direction is one-way: `audio.cpp` may include
`button_fsm.h`; **`button_fsm.cpp` must never include `audio.h`**. The main loop
orchestrates, as it does today.

Two calls join `loop()`, immediately after `webTick()` and **before** the
physical/virtual button merge, so a beat received this iteration reaches
`buttonFsmTick()` in the same iteration and a disarm POST closes the valve in the
same iteration:

```cpp
M5.update();
webTick();
audioTick();      // drain socket (bounded), age staleness + limiter
audioFireTick();  // decide + inject
FastLED.show();
```

The drain is bounded at `AUDIO_MAX_DRAIN = 8` packets per loop so a flood can never
stall the DMX frame; excess is counted and surfaced in `/api/state`.

> ⚠️ **Polled `WiFiUDP`, never `AsyncUDP`.** AsyncUDP callbacks run on the LwIP task,
> and `buttonConsumePress()` is a non-atomic test-then-clear on a `volatile bool`. A
> press injected between the test and the clear would be silently lost.

---

## The limiter

Three independent gates. **`cooldownMs` is never modified** — the operator's stored
value stays intact and is back in force the instant audio goes stale or disarms.

**Gate A — minimum inter-trigger interval.** `minGapMs`, default 200 ms, so at most
five shots per second. Drop mode uses its own longer `dropGapMs`.

**Gate B — rolling open-time budget.** An integer leaky bucket, no ring buffer, no
float, wrap-safe via unsigned `now - t0`. Long-run duty converges exactly to
`dutyPct`.

**Gate C — burst ceiling.** Bucket capacity is hard-ceilinged at
`AUDIO_BURST_CEIL_MS = 3000` regardless of configuration, because **3000 ms is
exactly what one default manual FIREBALL press already delivers**. The rule: nothing
audio-driven may deliver more propane in one burst than a single operator press can.

> ⚠️ **A leaky bucket alone is not safe.** At a legal maximum configuration it would
> permit tens of seconds of near-continuous propane before filling. The ceiling is
> what makes it safe and **must not be made configurable**.
>
> With the defaults, 40 % × 10 s = 4000 ms, but the ceiling clamps capacity to
> 3000 ms. Long-run duty is still 40 %; the maximum burst is 3.0 s.

Grants **shorten** rather than refuse as the budget depletes, so the feature degrades
into shorter pops instead of dropping out. A grant below one DMX frame returns 0
instead, because the `fsmConsumeFirePending()` latch would still put a full 50 ms
frame of gas on the wire and the budget would under-count.

### Accounting is measured, not assumed

One line in the DMX frame block, immediately before `dmxUpdate()`:

```cpp
audioNoteFrame(purge || (firing && mgOn)
               || (confluenceConfig.connected && morseActive()));
```

A fixture holds its last commanded byte until the next frame, so an open frame **is**
50 ms of gas. Frames × 50 ms is real open time.

The morse term matters: `morseTick()` drives confluence CH1 directly and is not gated
on `firing`, so omitting it would let a long message burn uncounted.

This counts **every** source — physical button, Test Fire, morse, purge — because
propane is physical. A long purge therefore locks audio out for the remainder of the
window, which is correct. **Manual fire is accounted but never gated**; the limiter
must not refuse an operator's deliberate press.

### Cooldown skip

Audio modes need the FSM back at IDLE without waiting out a lockout meant for manual
fire. One new primitive, mode-agnostic and unable to open a valve:

```cpp
// Cut a post-fire lockout short. Only ever leaves END_CUE or COOLDOWN early.
// Does NOT touch g_firePending — a frame of valve-open still owed is never swallowed.
void fsmSkipCooldown() {
  if (fsmState == FSM_END_CUE || fsmState == FSM_COOLDOWN) enterState(FSM_IDLE);
}
```

> ⚠️ **Scoped to audio-initiated shots only.** `audio.cpp` tracks
> `g_lastShotWasAudio` and skips only a lockout left by a shot it started. Without
> that scoping the operator's *physical* button would also lose its lockout whenever
> the device sat in an audio mode.

### Fail-safe matrix

| Condition | Action |
|---|---|
| No packet for `staleMs` | Zero features; lights fall back to plain themes; clear latches; open shot gets an immediate `buttonInjectRelease()`; all grants return 0 |
| Not armed, or not an audio mode | Grants return 0; lights still react; leaving an audio mode forces release |
| Boots into a persisted audio mode | Armed is false and no packets, so nothing fires; `mode` is clamped to `0..6` on load |
| Operator disarms mid-burn | Release injected in the same loop iteration; valve closed within one frame |
| `/api/button/reset` | Also calls `audioAbortShot()` — not disarm, since tests reset constantly |
| OTA | `safeToStart()` refuses while armed; `forceEverythingClosed()` disarms first |
| Another source owns the valve | Audio does not inject; no fighting over the FSM |
| Flood, rogue sender, Echo reboot | Bounded drain; peer lock releases on staleness so a reboot self-heals; extra senders counted as `bad` |

**Hard invariant: no UDP path may ever reach `purgeStart()`.** It holds all five
valves open with no limit.

---

## Modes

Modes 3–6 join `buttonConfig.mode`, which stays a plain `uint8_t`. All four are
release-terminated, so `audio.cpp` owns shot length via the grant with
`fireDurationMs` as the untouched hard backstop.

```cpp
static inline bool modeClosesOnRelease(uint8_t m) {
  return m == 1 || m == 2 || (m >= 3 && m <= 6);
}
```

| Mode | Trigger | Release |
|---|---|---|
| 3 beat pop | Predicted `nextBeat - leadMs`, confidence-gated; reactive edge as fallback | Grant expiry |
| 4 sustained bass | `bass >= bassOn` while IDLE | `bass < bassOff` (hysteresis) or expiry |
| 5 drop only | `BIGHIT` flag, or controller-side transient detect | Grant expiry |
| 6 machine gun on beat | Predicted grid, phase-anchored and lead-compensated | Silence, grid lost, or expiry |

Modes 4 and 5 are reactive by nature — sustained bass is a level rather than an
instant, and a drop is by definition unpredictable. Only 3 and 6 apply `leadMs`.

**Mode 6 replaces the `mgOn` expression, it does not reuse it.** The existing mode-2
gate is anchored to boot via `millis() % period`, which has no relationship to the
music:

```cpp
} else if (buttonConfig.mode == 6) {
  mgOn = audioBeatGate(buttonConfig.machineGunBurstMs);  // beat-anchored
}
```

At 20 Hz the open window quantises to 50 ms, so `machineGunBurstMs` below ~60 ms
lands 0 or 1 frames unpredictably. The UI hints a 100 ms floor for mode 6.

**Mode 5's transient detector runs controller-side as well as honouring the Echo's
flag** — a single-pole slow average of `level` with a threshold on the positive
difference. That makes the mode testable before the Echo exists and stops a weak
onset detector from killing it.

---

## Lights

`applyAudioLook()` is a `static inline` beside `applyFireLook()`, called after
`themeRender()` and **underneath** the fire, end-cue and purge chain — so every
existing override behaves exactly as before and no existing DMX test changes. It is
gated on `audioFresh()` alone: no arm check, no mode check.

> ⚠️ **Composition is `max()`, additive — never multiplicative.** The `green`, `blue`
> and `fire` gradient themes return a fully zeroed `TowerState` for 3200 ms of every
> 4000 ms cycle, so a multiplicative modulation is invisible 80 % of the time. An
> additive glow lights the blank phase and can never dim the bright phase.

Colour reuses `buttonConfig.fireUpR/G/B`, so there are no new colour keys and the
glow is the same amber the uplight snaps to when a valve opens — audio modulation
reads as the fire breathing before it fires.

`audlmode` selects **0 off**, **1 pulse** (uplight tracks the envelope, strips get a
smaller accent), or **2 band** (bass→`ur`, mid→`ug`, treble→`ub`, strips keep the
theme).

> ⚠️ **Photosensitivity.** Every audio light term passes through a fast-attack,
> slow-decay envelope with a hard-coded, non-configurable `AUDIO_LIGHT_DECAY_MS = 180`
> floor. No hard square gate anywhere. Same reasoning as the existing rule that the
> uplight does not strobe with `mgOn`.

**`themeRender()` is not touched.** It is a pure function mirrored line-for-line in
`tools/web-preview/simulator.html` and kept in lock-step by `/web-sync`; adding audio
there would need a JS port for every tweak, and drift is a silent visual bug. A new
`"audio"` theme would also be *per-tower*, turning one audio state into four
independent opt-ins. **Nothing audio-related is ported into `simulator.html`.**

---

## Web UI

A new **Audio** tab after Button Config, authored in `tools/web-preview/index.html`
and ported into `buildPage()` by `/web-sync`:

- **Link status** — FRESH/STALE pill, peer IP, packets/sec, seq gaps, bad count, band
  meters, a beat blinker, and a **budget bar** showing `dutyUsedMs / dutyCapMs`.
- **Arm row** — a latching control, **not** `setupHold()`, which is press-and-hold.
  Rendered from `state.audio.armed`, with `/api/state` re-read after each POST.
- **Safety limits** fieldset — shot ms, min gap, duty %, duty window, max single open,
  lead ms; each renders its computed consequence in prose underneath.
- **Response** fieldset — bass on/off, beat strength minimum, drop threshold, drop
  gap, drop shot, light mode, light depth.

Button Config gains modes 3–6, and the `mgRow` visibility test becomes
`value === '2' || value === '6'`.

`POST /set` gains `target=audio`. Unlike `target=button`, **every field is clamped**;
`bassOff` is additionally forced below `bassOn`. The branch falls through to the
shared `storageSave()` tail, so persistence is free.

**Fixed in passing:** `buttonConfig.mode` was written unvalidated and persists to NVS.
Harmless with three modes, but it now selects propane behaviour, so it is clamped to
`0..6` in both `handleSet()` and `storageLoad()`.

### `/api/state`

A new `audio` block, inserted **before** the `dmx` block so the `]}}` terminator is
untouched:

```json
"audio":{"armed":false,"fresh":false,"peer":"192.168.4.7","port":4210,
 "ageMs":18,"pps":40,"packets":12345,"gaps":2,"bad":0,
 "bass":12,"mid":40,"treble":8,"level":33,"bpm":124,"beatMs":484,
 "predicting":true,"shotActive":false,"dutyUsedMs":0,"dutyCapMs":3000,
 "cfg":{ ... }}
```

`s.reserve(2048)` grows to 3072 and `buildPage()`'s `s.reserve(36000)` to 46000.

### Endpoints

`POST /api/audio/arm` and `POST /api/audio/disarm`, registered explicitly as
`HTTP_POST` beside the purge routes.

> ⚠️ `onNotFound` 302s unregistered routes to `/` instead of 404ing, so a typo'd route
> looks like success to `fetch`. **Tests assert the resulting state change, never the
> HTTP status.**

**There is no `/api/audio/inject` endpoint.** pytest is on the AP and sends real UDP
from stdlib `socket`, exercising the real parser, peer lock and length checks. An HTTP
inject route would be a second unauthenticated path to propane existing only for
tests.

---

## Persistence

Fifteen new global NVS keys in the existing `dmxfire` namespace, all `aud`-prefixed
and within the 15-character limit. Load and save are strictly mirrored; defaults live
only in the `getX(key, default)` call, as elsewhere.

| key | type | default | clamp |
|---|---|---|---|
| `audshot` | `uint16_t` | 150 | 50–2000 |
| `audgap` | `uint16_t` | 200 | 100–5000 |
| `audduty` | `uint8_t` | 40 | 0–50 (0 = lights only) |
| `audwin` | `uint16_t` | 10000 | 2000–60000 |
| `audmaxopen` | `uint16_t` | 1000 | 50–3000 |
| `audlead` | `uint16_t` | 120 | 0–500 |
| `audstale` | `uint16_t` | 500 | 100–2000 |
| `audbasson` | `uint8_t` | 170 | 0–255 |
| `audbassoff` | `uint8_t` | 140 | 0–255, forced below `audbasson` |
| `audbeatmin` | `uint8_t` | 90 | 0–255 |
| `auddropmin` | `uint8_t` | 200 | 0–255 |
| `auddropgap` | `uint16_t` | 3000 | 500–30000 |
| `auddropshot` | `uint16_t` | 400 | 50–2000 |
| `audlmode` | `uint8_t` | 1 | 0–2 |
| `audldepth` | `uint8_t` | 150 | 0–255 |

Existing devices pick up the defaults on load — no `--erase` needed. Clamp constants
live in `audio.h` so the spec, the slider ranges and the handler cannot drift.

**`armed` is never persisted.** It is RAM-only and false on every boot.

> ⚠️ **`audstale` is 500 ms, not the 250 ms first proposed.** ESP32 SoftAP UDP does
> not deliver smoothly: packets cluster and burst, with documented delays of
> 200–300 ms and outliers past 450 ms. A 250 ms timeout trips on ordinary WiFi
> behaviour and cuts fire out mid-set. 500 ms is still far shorter than any shot's
> grant, so a dead link cannot hold a valve open — staleness only blocks new triggers.

---

## Tests

Host tooling lives in `tools/audio-sim/`: `audio_packet.py` is the canonical encoder
and `send_features.py` the CLI transmitter. `conftest.py` puts that directory on
`sys.path` so pytest and the CLI share exactly one encoder — a second copy would
drift from the firmware struct silently.

New in `tests/test_audio.py`. Safety group:

- **`test_audio_never_fires_when_disarmed`** — mode 3, disarmed, three seconds of
  beats, polled throughout: every valve channel stays 0.
- **`test_audio_never_fires_when_stale`** and
  **`test_audio_never_fires_in_non_audio_mode`** — the other two arms of the gate.
- **`test_audio_min_interval_enforced`** — beats at 20 Hz for five seconds; rising
  edges on CH8 stay at or below `5000/minGapMs + 1`.
- **`test_audio_duty_budget_caps_open_time`** — samples CH8 every 25 ms and asserts
  both the measured open fraction and the device's self-reported `dutyUsedMs`, since
  polling `/api/state` perturbs the loop it measures.
- **`test_audio_duty_budget_ignores_cooldown_zero`** — same with `cooldownMs=0`,
  `endCueMs=0`, `fireDurationMs=10000`. The exact gap this feature opens.
- **`test_audio_burst_ceiling_is_three_seconds`** — a configuration whose bucket would
  allow 30 s still never opens longer than ~3 s.
- **`test_audio_manual_fire_consumes_the_budget`** — proves accounting is physical,
  not per-source.
- **`test_audio_disarm_closes_an_open_valve`**, **`_mode_change_`**,
  **`_stale_mid_shot_`** — all close within ~150 ms.
- **`test_audio_ota_refused_while_armed`**,
  **`test_audio_flood_does_not_stall_the_dmx_loop`**,
  **`test_button_mode_is_clamped`**, **`test_audio_config_is_clamped`**,
  **`test_audio_routes_are_registered`**.

Prediction group:

- **`test_audio_fires_ahead_of_the_beat`** — with `audlead=200` and a steady 120 BPM
  grid, the CH8 rising edge lands ~200 ms *before* each beat instant.
- **`test_audio_prediction_needs_tempo_confidence`** — jittered intervals fall back to
  reactive edges rather than firing into silence.
- **`test_audio_prediction_stops_when_grid_dies`** — at most one predicted shot
  follows the last beat.
- **`test_audio_predicted_shot_still_obeys_the_limiter`** — prediction with the budget
  exhausted is still refused.
- **`test_audio_stale_default_survives_wifi_clustering`** — a 300 ms gap, inside the
  documented SoftAP burst range, does not drop `fresh`.

Behaviour group: **`test_audio_beat_pop_fires_once_per_beat`**,
**`_respects_beat_strength`**, **`test_audio_sustained_bass_holds_then_releases`**,
**`test_audio_drop_mode_ignores_ordinary_beats`**,
**`test_audio_machine_gun_locks_to_the_beat`** (contiguous open-group count tracks
beat count, which is what distinguishes an anchored phase from a free-running one),
**`test_audio_lights_react_when_fresh_but_disarmed`**, and
**`test_audio_light_glow_survives_the_gradient_blank`** (the uplight is nonzero on
every sample across a full 4000 ms cycle — a multiplicative implementation fails this
immediately).

---

## Commissioning

**`audlead` must be measured, not guessed.** Gas transit and ignition are the largest
and most rig-specific term in the latency chain. Fire a single shot with the
controller's onboard LED in frame, film at 120 fps or higher, count frames from LED to
visible flame, and set `audlead` to that total.

**The Echo must call `WiFi.setSleep(false)`.** Default modem sleep wakes the radio
only on DTIM beacons, typically every 100–300 ms, queueing packets for exactly that
long. This costs roughly 145 mA and is not optional for a real-time link.

---

## Non-goals

- **No authentication on the audio endpoints.** Consistent with every existing
  endpoint, but it means the AP password is the only thing between a guest's phone and
  armed propane. Worth a password-rotation habit for events.
- **No arm auto-expiry.** Arming persists until cleared or rebooted. Staleness and the
  limiter bound the consequences; an `audArmTimeoutMs` is cheaper to add now than to
  retrofit if that changes.
- **No gating of the physical button.** The limiter accounts manual fire but never
  refuses it. Refusing a deliberate operator shot is a much larger behaviour change.
- **No frame-level gas gate.** `audioFireTick()` runs every loop iteration, so a grant
  expiry closes the valve within one iteration and the next frame is closed — worst
  case one 50 ms frame, which is the bus floor. A second gate in the hottest,
  most safety-critical block buys nothing.
- **No DMX rate increase.** The 20 Hz bus is a deliberate flicker fix; see
  [spec-rapid-retrigger.md](spec-rapid-retrigger.md).
- **No FFT.** Per-band RMS energy flux is roughly 10× cheaper than spectral flux and is
  the recommended choice for percussive material on CPU-constrained real-time systems.
  Three band-pass biquads with envelope followers cover bass/mid/treble. The packet
  format is analyser-agnostic, so this can change without touching the controller.
- **No audio in `simulator.html`.** It mirrors `themeRender()` only.
