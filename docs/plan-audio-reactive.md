# Audio-reactive fire and lights

## Context

The rig currently fires propane from one physical button (GPIO39) through a 4-state FSM, and renders lights from pure per-frame theme functions. There is no audio input anywhere in the tree — `grep` for `udp|audio|artnet` returns nothing but two prose mentions.

The goal is to make the towers respond to music: **lights track the audio continuously, and fire triggers off the bass in four selectable modes.** An M5Stack Atom EchoS3R joins the controller's SoftAP, runs its own analysis, and streams a small UDP feature packet at 40 Hz. The Echo stays a separate device — it can't be a peripheral (its mic is on internal I2S, and Grove carries only 2 signals), and keeping it separate means the mic can sit where the music is while the controller stays at the valve manifold.

**The load-bearing problem this plan solves:** audio-driven fire needs `cooldownMs = 0` to sync to a beat at all, and `cooldownMs` is the *only* rate lockout in the firmware today. Removing it leaves propane duty bounded by nothing but `fireDurationMs` and the ~100 ms bus floor — roughly 10 shots/sec, sustained, indefinitely. So the audio path must bring its own rate authority. That limiter, not the packet format or the UI, is the core of this work.

Related: [spec-rapid-retrigger.md](spec-rapid-retrigger.md), [spec-machine-gun.md](spec-machine-gun.md), [spec-purge-accumulator.md](spec-purge-accumulator.md)

---

## Decisions already made

| Decision | Choice |
|---|---|
| Mode model | New modes **3–6** in the existing `buttonConfig.mode` list (plain `uint8_t`, no enum) |
| Behaviours | All four: beat pop, sustained bass, drop-only, machine-gun-on-beat |
| Lights vs fire | Lights react whenever packets are **fresh**; fire requires fresh **and** armed **and** an audio mode |
| Duty ceiling | **40 % over a 10 s rolling window, 200 ms minimum between shots** |
| Arm dead-man | Stays armed until cleared or rebooted (RAM-only flag) — so **staleness is the primary dead-man** |
| Transport | Polled `WiFiUDP` drained in `loop()`, **not** `AsyncUDP` |

### Two corrections to earlier assumptions

- **The DMX bus is 20 Hz, not 50 Hz.** `dmx.h:43` sets `DMX_FRAME_INTERVAL_MS = 50` — a 50 ms period, deliberately slow as a flicker fix (commit `41404cf`). Valve bytes change at most 20×/sec; shot-cycle floor is ~100 ms. CLAUDE.md's "50 Hz" is stale and should be corrected in passing. **Do not raise the frame rate** — `spec-rapid-retrigger.md` lists that as a non-goal.
- **M5Unified exists at two versions.** The repo carries `libraries/M5Unified` = **0.2.13**; the sketchbook that `arduino-cli` actually resolves has **0.2.4**. The Echo needs ≥ 0.2.8. Reconciling this re-versions M5Unified for the controller too, so it must be **its own commit** with its own `scripts/ota.sh` + `runDiagnostics()` verification — never folded into this feature.

---

## Wire format

**Port 4210/UDP.** DNS already owns 53 on the AP (`web.cpp:876`), DHCP 67, HTTP 80; lwIP ephemerals start at 49152.

Fixed **24-byte packet**, little-endian, `__attribute__((packed))` with a `static_assert` on the size. Fields: `magic[4]="DFAU"`, `version`, `flags` (b0 BEAT, b1 BIGHIT, b2 CLIP, b3 SILENCE, b4 MICFAULT), `session` (random per Echo boot), `seq`, then `bass`/`mid`/`treble`/`level`/`beatStrength`/`bpm` as `uint8`, then `sinceBeatMs`, `frameMs`, `reserved`.

No CRC (lwIP checksums UDP already) and no HMAC — the AP is WPA2 and `/api/button/press` is already unauthenticated, so a shared secret here would be theatre. The receiver enforces what's cheap and real: exact length, magic, version, source in `192.168.4.0/24`, monotonic `seq` within a `session`, and a first-sender peer lock.

**`sinceBeatMs` is the reason the packet isn't smaller.** It carries the Echo's analysis + send latency so the controller can anchor the beat grid to the *acoustic* instant rather than to packet arrival. It is attacker- and bug-controlled, so **clamp it to 200 ms** — without that clamp a bogus value parks the beat anchor in the past and the mode-6 gate misbehaves. The clamp is load-bearing.

> ⚠️ **40 Hz into a 20 Hz consumer means beats are edge events, not samples.** The drain must **OR-latch `flags` and take `max(beatStrength)` across every packet since the last read**, while taking band energies from the *newest* packet. Naively "using the last packet" silently drops about half of all beats.

The 40 Hz rate buys latency, not resolution: a 25 ms analysis hop halves worst-case beat-to-packet delay, and one lost packet costs 25 ms of staleness instead of 50.

---

## Latency and beat prediction

> This section was added after researching how audio-reactive systems actually behave. It is the one place where the original design was **wrong**, not merely incomplete.

### The chain is longer than the perceptual window

| Stage | Latency |
|---|---|
| Mic → onset detected (analysis window + hop) | 25–50 ms |
| Echo → controller over WiFi | 0.5–2 ms typical; **bursts of 10–20 ms, SoftAP outliers of 200–460 ms** |
| Controller loop → next DMX frame (20 Hz gate) | 0–50 ms (avg 25) |
| DMX frame transmit + decoder | 3–10 ms |
| Solenoid electrical + mechanical | 10–100 ms |
| Gas transit + ignition to visible flame | **unmeasured on this rig** — the one adjacent industrial figure found was 150–250 ms |

Realistic typical total is **~100–250 ms**, and the tail is much worse.

Against that, the perceptual research: the window supporting apparent audiovisual synchrony is roughly **200 ms at 1 Hz, narrowing to ~100 ms at 4 Hz** — so around 150 ms at club tempo. Worse, the asymmetry runs against us: listeners are *more* sensitive to audio-leading-visual than the reverse, which is exactly the direction a reactive trigger errs.

> ⚠️ **A purely reactive trigger cannot land on the beat.** It will read as "fire follows the music," not "fire is on the beat." Every stage above is additive and none of them are optional.

### The fix: predict the next beat, fire early

This is standard practice, not an invention — real-time beat trackers expose look-ahead precisely to compensate for "audio processing, network communication, or input controller lags." The plan already carries every ingredient (`bpm`, `sinceBeatMs`, and a computed `beatIntervalMs`); it simply triggers on the wrong edge.

```
nextBeatMs   = g_beatAnchorMs + g_beatIntervalMs
fireAtMs     = nextBeatMs - audioConfig.leadMs      // leadMs = measured total chain
```

Modes 3 and 6 schedule against `fireAtMs` instead of firing on the `BEAT` latch. One new calibratable config value, `audlead`, default **120 ms**, range 0–500.

Two guards, both load-bearing:

- **Prediction degrades detection accuracy** — the research is explicit about that trade-off. So predict only when the tempo estimate is confident (a stable `beatIntervalMs` across ≥ 4 beats, and `bpm != 0`); otherwise fall back to the reactive edge. A wrong prediction fires propane at a moment with no beat under it.
- **Never predict more than one beat ahead**, and drop the schedule the moment the grid goes stale. A predicted trigger must still pass every limiter gate — prediction changes *when* a shot is requested, never *whether* it is allowed.

### Calibrate `leadMs`, don't guess it

Gas transit and ignition are the largest unknown term and are specific to this rig's plumbing. Measure it once: fire a single shot, film at 120 fps or higher with the DMX frame's onboard LED in frame, and count frames from LED to visible flame. Set `audlead` to that total. This belongs in the spec as a commissioning step.

### Lights and fire need different offsets

Lights have no mechanical or combustion delay — their chain is analysis + network + one DMX frame, roughly 50–100 ms. Fire carries the whole chain. Driving both from one trigger instant means one of them is wrong. `applyAudioLook()` should stay **reactive** (it is already, and correctly so), while only the fire path applies `leadMs`. Do not "fix" the lights by giving them the same lead.

---

## New module: `Test_Button_DMX/audio.h` / `audio.cpp`

Dependency direction is strictly one-way: **`audio.cpp` may include `button_fsm.h`; `button_fsm.cpp` must never include `audio.h`.** The main loop orchestrates, as it does today.

Public surface (abbreviated — full signatures in the spec):

```cpp
void audioSetup();      // udp.begin(), called from setup() after webSetup()
void audioTick();       // drain socket (bounded), age staleness + limiter
void audioFireTick();   // decide + inject; must run BEFORE the button merge

bool audioFresh(); bool audioArmed(); void audioArm(); void audioDisarm();
void audioAbortShot();  // kill the shot, leave armed alone
const AudioFeatures& audioSnapshot();
uint8_t audioEnvelope(); bool audioBeatGate(uint16_t onMs);

uint16_t audioLimiterGrant(uint16_t requestedMs);  // 0 = refuse
void     audioNoteFrame(bool anyValveOpen);        // ONCE per DMX frame
```

Two new calls in `loop()`, placed **immediately after `webTick()` and before the physical/virtual button merge**, so a beat received this iteration reaches `buttonFsmTick()` in the same iteration and a disarm POST closes the valve in the same iteration:

```
M5.update(); webTick(); audioTick(); audioFireTick(); FastLED.show(); ...
```

The drain is bounded at `AUDIO_MAX_DRAIN = 8` packets per loop so a flood can never stall the DMX frame; excess is counted and surfaced.

---

## The safety limiter

Three independent gates. The FSM's `cooldownMs` is **never modified** — the operator's stored value stays intact and is back in force the instant audio goes stale or disarms.

**Gate A — minimum inter-trigger interval.** `minGapMs`, default **200 ms** (≤ 5 shots/sec). Drop mode has its own longer `dropGapMs`, default 3000 ms.

**Gate B — rolling open-time budget**, an integer leaky bucket (no ring buffer, no float, wrap-safe via unsigned `now - t0`). `dutyPct` default **40**, `dutyWinMs` default **10000**. Long-run duty converges exactly to `dutyPct`.

**Gate C — burst ceiling and max single open.** The bucket capacity is hard-ceilinged at `AUDIO_BURST_CEIL_MS = 3000` regardless of config, because **3000 ms is exactly what one default manual FIREBALL press already delivers** (`fireDurationMs` default 3000). The rule: *nothing audio-driven may deliver more propane in one burst than a single operator press already can.* `maxOpenMs` (default 1000) bounds any single continuous open.

> ⚠️ **A leaky bucket alone is not safe.** At a legal max config the bucket would permit tens of seconds of near-continuous propane before filling. The burst ceiling is what makes it safe, and it must not be made configurable.
>
> Note the interaction with the chosen numbers: 40 % × 10 s = 4000 ms, but the ceiling clamps bucket capacity to **3000 ms**. Long-run duty is still 40 %; the maximum *burst* is 3.0 s, not 4.0 s.

**Accounting is measured, not assumed.** Exactly one new line in the DMX frame block, immediately before `dmxUpdate()`:

```cpp
audioNoteFrame(purge || (firing && mgOn) || (confluenceConfig.connected && morseActive()));
```

A DMX fixture holds its last commanded byte until the next frame, so an open frame *is* 50 ms of gas — frames × 50 ms is real open time. The morse term matters: `morseTick()` drives confluence CH1 directly and is **not** gated on `firing`, so omitting it would let a long message burn uncounted.

This counts **every** source — physical button, Test Fire, morse, purge — because propane is physical. Consequence to document: a long purge locks audio out for the rest of the window. That is correct. **Manual fire is accounted but never gated** — the limiter must not refuse an operator's deliberate press.

### Cooldown skip, scoped

Audio modes need the FSM to return to IDLE without waiting out a lockout meant for manual fire. One new mode-agnostic primitive in `button_fsm.cpp` that **cannot open a valve**:

```cpp
// Cut a post-fire lockout short. Only ever leaves END_CUE or COOLDOWN early.
// Does NOT touch g_firePending — a frame of valve-open still owed is never swallowed.
void fsmSkipCooldown() {
  if (fsmState == FSM_END_CUE || fsmState == FSM_COOLDOWN) enterState(FSM_IDLE);
}
```

> ⚠️ **Scope this to audio-initiated shots only.** Track `g_lastShotWasAudio` in `audio.cpp` and call `fsmSkipCooldown()` only when the lockout being skipped came from a shot audio itself started. Without that scoping, the operator's *physical* button also loses its lockout whenever the device sits in an audio mode — a real weakening of existing behaviour, and an unnecessary one.

### Fail-safe matrix

| Condition | Action |
|---|---|
| No packet for `staleMs` (**500 ms** — see below) | zero features → lights fall back to plain themes; clear latches; open shot gets an immediate `buttonInjectRelease()`; all grants return 0 |
| Not armed / not an audio mode | grants return 0; lights still react; leaving an audio mode forces release |
| Boots into a persisted audio mode (`btnmode` holds 3–6) | armed is false and no packets ⇒ nothing fires; `storageLoad()` also clamps `mode > 6 → 0` |
| Operator disarms mid-burn | release injected in the same loop iteration; valve closed within one frame (≤ 50 ms) |
| `/api/button/reset` | also calls `audioAbortShot()` — **not** disarm, since tests reset constantly |
| OTA | `safeToStart()` gains an armed check; `forceEverythingClosed()` calls `audioDisarm()` first |
| Another source owns the valve (`fsmState != IDLE`) | audio does not inject — no fighting over the FSM |
| Packet flood / rogue sender / Echo reboot | bounded drain; peer lock releases on staleness so a reboot self-heals; extra senders counted as `bad` |

**Hard invariant: no UDP path may ever reach `purgeStart()`.** It holds all five valves open with no limit.

---

## The four modes

`3 = beat pop`, `4 = sustained bass`, `5 = drop only`, `6 = machine-gun on beat`. All four are release-terminated so `audio.cpp` owns shot length via the grant, with `fireDurationMs` as the untouched hard backstop.

**Change 1** — `button_fsm.cpp:85`, replace the literal with a named predicate, keeping it one site:

```cpp
static inline bool modeClosesOnRelease(uint8_t m) { return m == 1 || m == 2 || (m >= 3 && m <= 6); }
```

**Change 2** — extend the `mgOn` block in `.ino` (currently `if (buttonConfig.mode == 2)` at `Test_Button_DMX.ino:135`) with an `else if (mode == 6)` arm calling `audioBeatGate(machineGunBurstMs)`. The existing mode-2 expression is anchored to boot via `millis() % period`, so it **cannot** be reused for beat-locking — mode 6 needs a phase anchor derived from `rxMs - sinceBeatMs`, with the grid failing closed when no beat has landed for two intervals.

| Mode | Trigger | Release | Grant requested |
|---|---|---|---|
| 3 beat pop | **predicted** `nextBeat - leadMs`, confidence-gated; reactive `BEAT` latch as fallback | grant expiry | `shotMs` (150) |
| 4 sustained | `bass ≥ bassOn`, FSM IDLE | `bass < bassOff` (hysteresis) or expiry | `maxOpenMs` (1000) |
| 5 drop | `BIGHIT` flag **or** controller-side transient detect | grant expiry | `dropShotMs` (400) |
| 6 mg-on-beat | **predicted** grid, phase-anchored and lead-compensated | silence / grid lost / expiry | `maxOpenMs`, re-requested |

Modes 4 and 5 stay reactive by nature — sustained bass is a level, not an instant, and a drop is by definition unpredictable. Only 3 and 6 ride the beat grid, and only they apply `leadMs`.

Implement mode 5's transient detector **controller-side as well** (single-pole slow average of `level`, τ ≈ 2 s, threshold on positive difference — about 6 lines). It makes the mode testable from the simulator before the Echo exists and stops a weak Echo-side onset detector from killing the feature.

At 20 Hz the mode-6 open window quantises to 50 ms, so `machineGunBurstMs` below ~60 ms lands 0 or 1 frames unpredictably — default it to 150 and hint a 100 ms floor in the UI.

---

## Lights

`applyAudioLook(state, i)` as a `static inline` in the `.ino` directly below `applyFireLook()`, called after `themeRender()` and **underneath** the fire / end-cue / purge chain — so every existing override looks exactly as it does today and no existing DMX test changes. Gated on `audioFresh()` alone: no arm check, no mode check.

**Composition is `max()`, additive — never multiplicative.** This is the whole answer to the gradient themes: `green`/`blue`/`fire` return a fully zeroed `TowerState` for 3200 ms of every 4000 ms cycle, so anything multiplicative is invisible 80 % of the time. An additive glow lights the blank phase and can never dim the bright phase.

Colour reuses `buttonConfig.fireUpR/G/B` — zero new NVS keys, and the glow is the same amber the uplight snaps to when a valve opens, so audio modulation reads as the fire breathing before it fires.

Modes under `audlmode`: **0 off / 1 pulse** (uplight tracks the envelope, strips get a smaller accent) **/ 2 band** (bass→`ur`, mid→`ug`, treble→`ub`, strips keep the theme).

> ⚠️ **Photosensitivity.** Every audio light term passes through a fast-attack / slow-decay envelope with a hard-coded, non-configurable `AUDIO_LIGHT_DECAY_MS = 180` floor. No hard square gate anywhere. Same reasoning as `.ino`'s existing note that the uplight deliberately does not strobe with `mgOn`.

**Rejected:** adding an audio parameter to `themeRender()` or a new `"audio"` theme. `themeRender()` is a pure function mirrored line-for-line in `tools/web-preview/simulator.html` and kept in lock-step by `/web-sync`; every audio tweak would need a JS port, and drift there is a silent visual bug. A theme would also be *per-tower*, turning one audio state into four independent opt-ins. **Do not port anything audio-related into `simulator.html`.**

---

## Web UI, API, persistence

New **Audio** tab after Button Config, authored in `tools/web-preview/index.html` (source of truth) and ported to `web.cpp::buildPage()` via `/web-sync`. Contents: live link status (FRESH/STALE pill, peer IP, pps, seq gaps, bad count, band meters, beat blinker, **budget bar** showing `dutyUsedMs / dutyCapMs`), a latching arm row, a *Safety limits* fieldset that renders each setting's computed consequence in prose, a *Response* fieldset, and a one-click "use an audio-friendly theme" button that POSTs `target=all theme=candle`.

The arm row **must not** reuse `setupHold()` — that factory is press-and-hold, and arming is a server-side latch. Render from `state.audio.armed` and re-read `/api/state` after each POST.

Button Config tab: add options 3–6, and change the `mgRow` visibility test from `value==='2'` to `value==='2'||value==='6'`.

**Endpoints** — `POST /api/audio/arm` and `/api/audio/disarm`, registered explicitly as `HTTP_POST` next to the purge routes, following the house handler shape.

> ⚠️ `onNotFound` **302s unregistered routes to `/`** instead of 404ing, so a typo'd route looks like success to `fetch`. Tests must assert the resulting *state change*, never the HTTP status.

**No `/api/audio/inject` test endpoint.** pytest is on the AP and can send real UDP from stdlib `socket`, exercising the real parser, peer lock and length checks. An HTTP inject route would be a second unauthenticated path to propane existing only for tests.

`POST /set target=audio` — new `else if` branch before the numeric-tower fallback, every field `hasArg`-guarded and, unlike `target=button`, **every field clamped**. Falls through to the shared `storageSave()` tail so persistence is free. **Also fix in passing:** `buttonConfig.mode` is written unvalidated at `web.cpp:592` and now selects propane behaviour — clamp to `0..6` there and in `storageLoad()`.

`/api/state` — insert an `audio` block **before** the `dmx` block so the `]}}` terminator is untouched; grow `s.reserve(2048)` → 3072 and `buildPage()`'s `36000` → 46000.

**NVS** (namespace `dmxfire`, all keys ≤ 11 chars, `aud` prefix, load/save strictly mirrored):

`audshot` 150 · `audgap` **200** · `audduty` **40** · `audwin` **10000** · `audmaxopen` 1000 · `audbasson` 170 · `audbassoff` 140 · `audbeatmin` 90 · `auddropmin` 200 · `auddropgap` 3000 · `auddropshot` 400 · `audlmode` 1 · `audldepth` 150 · `audstale` **500** · `audlead` **120**

> ⚠️ **`audstale` is 500 ms, not the 250 ms originally planned.** ESP32 SoftAP UDP does not deliver smoothly: packets cluster and burst, with documented delays of 200–300 ms and outliers to 460 ms. A 250 ms timeout would trip on ordinary WiFi behaviour and cut fire out mid-set, which reads as a broken rig. 500 ms is still far shorter than any shot's grant, so a dead link cannot hold a valve open — staleness only blocks *new* triggers.

Clamp constants live in `audio.h` so the spec, the slider ranges and the handler cannot drift. **`armed` is never persisted.**

---

## Verification

**Host tooling** — `tools/audio-sim/audio_packet.py` (canonical encoder, stdlib only) plus `send_features.py`. `conftest.py` puts that directory on `sys.path` so pytest and the CLI share exactly one encoder; a second copy in `tests/` would drift from the firmware struct silently. CLI flags: `--pattern beat|bass|drop|sweep|silence|music`, `--bpm`, `--rate`, `--loss`, `--stop-after`, `--corrupt magic|version|short|long`, `--flood`, `--session`, `--seq-start`. `--pattern music` synthesises a 4/4 grid with kick-weighted bass, hats on 8ths and a `BIGHIT` every 32 bars — that one pattern is what makes the rig demoable with no Echo.

`tools/web-preview/server.py` gains `/api/audio/arm|disarm` mocks, a `target=audio` arm in `update_config()`, and an `audio` block in `build_state_json()` driven by a synthetic 120 BPM oscillator — without it the Audio tab can't be developed against the mock.

**`tests/test_audio.py`** — grouped by intent, following house style (`ch(state, n)`, `time.sleep(0.1)` after state changes, `set` sampling for animated behaviour). The safety group is the point of the suite:

- **`test_audio_never_fires_when_disarmed`** — mode 3, disarmed, 3 s of beats, poll throughout: every valve channel stays 0. The single most important test here.
- **`test_audio_never_fires_when_stale`** / **`test_audio_never_fires_in_non_audio_mode`** — the other two arms of the gate.
- **`test_audio_min_interval_enforced`** — beats at 20 Hz for 5 s; count rising edges on CH8; assert `≤ 5000/minGapMs + 1`.
- **`test_audio_duty_budget_caps_open_time`** — sample CH8 every 25 ms for 15 s; assert measured open fraction ≤ `dutyPct` + margin **and** cross-check the device's self-reported `dutyUsedMs`. Two independent views, because polling `/api/state` perturbs the loop it measures.
- **`test_audio_duty_budget_ignores_cooldown_zero`** — same with `cooldownMs=0, endCueMs=0, fireDurationMs=10000`. This is the exact gap the feature opens; the budget must still hold.
- **`test_audio_burst_ceiling_is_three_seconds`** — `dutyPct=50, dutyWinMs=60000`; longest continuous run never exceeds ~3 s + 1 frame.
- **`test_audio_manual_fire_consumes_the_budget`** — proves accounting is physical, not per-source.
- **`test_audio_disarm_closes_an_open_valve`** / **`_mode_change_`** / **`_stale_mid_shot_`** — all close within ~150 ms.
- **`test_audio_ota_refused_while_armed`**, **`test_audio_flood_does_not_stall_the_dmx_loop`**, **`test_button_mode_is_clamped`**, **`test_audio_config_is_clamped`**, **`test_audio_routes_are_registered`**.

Prediction group — new, and the reason `send_features.py` emits a *scheduled* grid rather than random beats:

- **`test_audio_fires_ahead_of_the_beat`** — `audlead=200`, steady 120 BPM; assert the CH8 rising edge lands ~200 ms *before* each packet's beat instant, not after. Directly proves lead compensation rather than assuming it.
- **`test_audio_prediction_needs_tempo_confidence`** — jittered, unstable beat intervals; assert the device falls back to reactive edges (no early firing) instead of firing into silence.
- **`test_audio_prediction_stops_when_grid_dies`** — beats stop mid-stream; assert at most one predicted shot follows and nothing after.
- **`test_audio_predicted_shot_still_obeys_the_limiter`** — prediction with `dutyPct` exhausted; assert grants are still refused. Prediction changes *when*, never *whether*.
- **`test_audio_stale_default_survives_wifi_clustering`** — sender pauses 300 ms mid-stream (inside the documented SoftAP burst range); assert `fresh` stays true and fire does not drop out.

Behaviour group: **`test_audio_beat_pop_fires_once_per_beat`**, **`_respects_beat_strength`**, **`test_audio_sustained_bass_holds_then_releases`**, **`test_audio_drop_mode_ignores_ordinary_beats`**, **`test_audio_machine_gun_locks_to_the_beat`** (assert contiguous open-group count ≈ beat count — this is what distinguishes an anchored phase from the free-running one), **`test_audio_lights_react_when_fresh_but_disarmed`**, and **`test_audio_light_glow_survives_the_gradient_blank`** (uplight nonzero on every sample across a full 4000 ms cycle — a multiplicative implementation fails this immediately).

Iterate with `scripts/ota.sh` (seconds over WiFi); `scripts/flash.sh` only for recovery. Preview UI work at `http://127.0.0.1:8123` via `scripts/web-debug.sh`.

---

## Phases

Each phase is independently uploadable and leaves the rig safe.

| # | Deliverable | Verified by |
|---|---|---|
| 0 | `docs/spec-audio-reactive.md` in house format | review |
| 1 | Wire format + host sender. **No firmware change.** | `tcpdump -X -i en0 udp port 4210` |
| 2 | Receive path only — socket, parser, peer lock, staleness, counters, config, NVS, `target=audio`, arm/disarm, `/api/state.audio`, Audio tab. **No fire, no lights.** | plumbing + freshness + flood tests |
| 3 | Lights only — `applyAudioLook()` | the two light tests |
| 4 | Limiter + accounting, **with no trigger source wired** | `test_audio_manual_fire_consumes_the_budget` — the budget is proven to measure real valve time *before* it may gate anything |
| 5 | Modes 3 and 5 + `modeClosesOnRelease()` + scoped `fsmSkipCooldown()` + the predictor and its confidence gate | negative gates, beat pop, min interval, drop, lead compensation |
| 6 | Modes 4 and 6 + `mgOn` extension + beat anchor | sustain, budget, burst ceiling, mg-locks-to-beat |
| 7 | Safety integration: `ota.cpp`, `/api/button/reset` → `audioAbortShot()`, mode clamps, `runDiagnostics()` section, conftest baseline, `/web-sync` port, docs | full suite |

> ### Stop line
> **After phase 7 the feature is complete and shippable with no Echo hardware, no Echo firmware, no `arduinoFFT`, and no M5Unified upgrade.** A laptop on the AP running `send_features.py --pattern music --bpm 128` drives the whole rig — lights and propane.

| # | Deliverable |
|---|---|
| 8 | Echo firmware `Atom_EchoS3R_Audio/` — AP join, mic capture, band split, beat detection, 40 Hz TX, LED link status. Validated against the `/api/state.audio` counters phase 2 already built. Prereq: the M5Unified 0.2.4 / 0.2.13 reconciliation, **as its own commit**. |
| 9 | Field tuning — thresholds, AGC, BPM tracking, and the `audlead` calibration below. |

> ⚠️ **The Echo must call `WiFi.setSleep(false)` (`WIFI_PS_NONE`).** Default modem sleep wakes the radio only on DTIM beacons, typically every 100–300 ms, which queues packets for exactly that long — the dominant cause of the SoftAP latency figures above. This costs ~145 mA and is non-negotiable for a real-time link. Even with it disabled, ESP-IDF's own issue tracker shows residual 10–20 ms burst behaviour that application code cannot tune away, which is why the receiver OR-latches beats and why `audstale` is 500 ms.

**`audlead` calibration** (phase 9, a commissioning step, not a code change): fire a single shot with the controller's onboard LED in frame, film at ≥ 120 fps, count frames from LED to visible flame, set `audlead` to that total. Gas transit and ignition are the largest and most rig-specific term in the chain, and guessing them defeats the prediction.

`arduinoFFT` is not installed, and for three bands it isn't needed — **and the research supports dropping it.** Per-band RMS energy flux (positive first differences with an adaptive threshold, no FFT) is roughly 10× cheaper than spectral flux and is specifically the recommended choice for percussive material on CPU-constrained real-time systems; multi-band onset detection is also what separates a low-frequency thump from a high-frequency click, which is exactly the bass/mid/treble split this packet carries. Three band-pass biquads plus envelope followers and per-band energy flux is the right analyser here. The packet format is analyser-agnostic either way.

---

## Non-goals

- **No auth on the audio endpoints.** Consistent with every existing endpoint, but it means the AP password is the only thing between a guest's phone and armed propane. Worth an explicit line in the spec and a password-rotation habit for events.
- **No arm auto-expiry.** Chosen deliberately; staleness plus the limiter bound the consequences. An `audArmTimeoutMs` is cheaper to add now than to retrofit if that changes.
- **No gating of the physical button.** The limiter accounts manual fire but never refuses it.
- **No frame-level gas gate.** `audioFireTick()` runs every loop iteration, so a grant expiry closes the valve within one iteration and the next frame is closed — worst case one 50 ms frame, which is the bus floor. A second gate in the hottest, most safety-critical block buys nothing.
- **No DMX rate increase.** The 20 Hz bus is a deliberate flicker fix.
- **No audio in `simulator.html`.** It mirrors `themeRender()` only.
