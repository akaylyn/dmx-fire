"""Audio-reactive fire and lights.

Requires the device on the AP (192.168.4.1) and the workstation joined to it, so
UDP on 4210 actually reaches the firmware. Tests send REAL packets through the
real parser rather than an HTTP inject endpoint — there is deliberately no such
endpoint, because it would be a second unauthenticated path to propane that
exists only for tests.

The safety group is the point of this file. Everything else is behaviour.
"""

from __future__ import annotations

import socket
import time

import pytest

import audio_packet as ap
from audio_sender import AudioSender

TOWER_FIRE_CH = [8, 23, 38, 53]
CONFLUENCE_FIRE_CH = 1


def ch(state, dmx_ch):
    return state["dmx"]["ch"][dmx_ch - 1]


def any_valve_open(state):
    return any(ch(state, c) for c in TOWER_FIRE_CH) or ch(state, CONFLUENCE_FIRE_CH) != 0


def sample_valves(device, seconds, interval=0.025):
    """Poll CH8 for `seconds`, returning [(monotonic, value), ...]."""
    out = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        out.append((time.monotonic(), ch(device.get_state(), TOWER_FIRE_CH[0])))
        time.sleep(interval)
    return out


def rising_edges(samples):
    edges, prev = [], 0
    for t, v in samples:
        if v and not prev:
            edges.append(t)
        prev = v
    return edges


def open_runs(samples):
    """Contiguous open groups as (start, end) monotonic pairs."""
    runs, start, prev = [], None, 0
    for t, v in samples:
        if v and not prev:
            start = t
        elif not v and prev and start is not None:
            runs.append((start, t))
            start = None
        prev = v
    if start is not None:
        runs.append((start, samples[-1][0]))
    return runs


def host_ip(device):
    return device.host


def settle(device, timeout=2.0):
    """Wait for the valve to actually read closed before sampling.

    Without this a shot still open from the previous test shows up as the first
    sample, and rising_edges() counts it as an edge because `prev` starts at 0.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ch(device.get_state(), TOWER_FIRE_CH[0]) == 0:
            return
        time.sleep(0.05)


# ===========================================================================
# Plumbing
# ===========================================================================


def test_audio_state_shape(device):
    """Every documented key exists. Guards the s.reserve(2048)->3072 growth — a
    truncated body would surface here as a KeyError rather than as a mystery."""
    a = device.audio()
    for key in ("armed", "fresh", "peer", "port", "ageMs", "pps", "packets", "gaps",
                "bad", "floods", "bass", "mid", "treble", "level", "bpm", "beatMs",
                "beat", "confident", "shotActive", "dutyUsedMs", "dutyCapMs", "cfg"):
        assert key in a, f"missing /api/state.audio.{key}"
    for key in ("shotMs", "minGapMs", "dutyPct", "dutyWinMs", "maxOpenMs", "leadMs",
                "staleMs", "bassOn", "bassOff", "beatMin", "dropMin", "dropGapMs",
                "dropShotMs", "lightMode", "lightDepth"):
        assert key in a["cfg"], f"missing /api/state.audio.cfg.{key}"
    assert a["port"] == ap.DEFAULT_PORT


def test_audio_config_round_trip(device):
    device.set_audio(audShotMs=222, audMinGapMs=333, audDutyPct=17, audLeadMs=180)
    cfg = device.audio()["cfg"]
    assert (cfg["shotMs"], cfg["minGapMs"], cfg["dutyPct"], cfg["leadMs"]) == (222, 333, 17, 180)


def test_audio_config_is_clamped(device):
    """These govern propane duty. A bad POST must not widen the safety envelope."""
    device.set_audio(audDutyPct=200, audMinGapMs=0, audShotMs=99999, audMaxOpenMs=99999)
    cfg = device.audio()["cfg"]
    assert cfg["dutyPct"] <= 50
    assert cfg["minGapMs"] >= 100
    assert cfg["shotMs"] <= 2000
    assert cfg["maxOpenMs"] <= 3000


def test_audio_bass_hysteresis_is_enforced(device):
    """bassOff must stay below bassOn or the valve chatters at the threshold."""
    device.set_audio(audBassOn=150, audBassOff=200)
    cfg = device.audio()["cfg"]
    assert cfg["bassOff"] < cfg["bassOn"]


def test_button_mode_is_clamped(device):
    """mode selects propane behaviour now. Direct guard on the unvalidated write."""
    device.set_button(mode=99, fireDurationMs=500, cooldownMs=2000)
    assert device.get_state()["button"]["mode"] == 0


def test_audio_routes_are_registered(device):
    """Asserts the STATE CHANGE, not the HTTP status.

    onNotFound 302s unregistered routes to "/", so an unregistered endpoint would
    return 200 after the redirect and look like success. Only the flag proves it.
    """
    device.audio_arm()
    assert device.audio()["armed"] is True
    device.audio_disarm()
    assert device.audio()["armed"] is False


# ===========================================================================
# Freshness
# ===========================================================================


def test_audio_starts_disarmed(device):
    assert device.audio()["armed"] is False


def test_audio_packets_make_state_fresh(device):
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=10, level=10)
        time.sleep(0.6)
        a = device.audio()
    assert a["fresh"] is True
    assert a["packets"] > 0
    assert a["pps"] > 0
    assert a["peer"] != "0.0.0.0"


def test_audio_goes_stale_when_sender_stops(device):
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(level=50)
        time.sleep(0.6)
        assert device.audio()["fresh"] is True
        tx.go_silent()
        time.sleep(1.2)          # staleMs is 500 in the baseline
        assert device.audio()["fresh"] is False


def test_audio_stale_default_survives_wifi_clustering(device):
    """A 300 ms gap is inside the documented SoftAP burst range and must NOT drop
    the link. This is why staleMs is 500 and not the 250 first proposed."""
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(level=50)
        time.sleep(0.6)
        tx.go_silent()
        time.sleep(0.30)
        tx.go_silent(False)
        time.sleep(0.15)
        assert device.audio()["fresh"] is True


def test_audio_reports_seq_gaps(device):
    before = device.audio()["gaps"]
    with AudioSender(host_ip(device)) as tx:
        tx.set_loss(0.3)
        time.sleep(2.0)
    assert device.audio()["gaps"] > before


def test_audio_rejects_bad_packets(device):
    before = device.audio()["bad"]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (host_ip(device).replace("http://", "").split("/")[0], ap.DEFAULT_PORT)
    try:
        sock.sendto(ap.pack(0, 1, magic=b"XXXX"), target)
        sock.sendto(ap.pack(1, 1, version=99), target)
        sock.sendto(b"\x00" * 12, target)
        sock.sendto(b"\x00" * 64, target)
        time.sleep(0.4)
    finally:
        sock.close()
    a = device.audio()
    assert a["bad"] >= before + 4
    assert a["fresh"] is False, "malformed packets must never mark the link fresh"


def test_audio_flood_does_not_stall_the_dmx_loop(device):
    """AUDIO_MAX_DRAIN bounds the work a flood can force. The device must still
    answer HTTP and still be writing DMX afterwards."""
    device.set_all_towers(theme="bright_white", brightness=255)
    with AudioSender(host_ip(device), rate=1500) as tx:
        tx.set_bands(level=200)
        time.sleep(2.0)
        state = device.get_state()          # must not time out
    assert state["dmx"]["ch"][TOWER_FIRE_CH[0] - 3] > 0, "DMX stopped updating under flood"


# ===========================================================================
# Safety — the group that matters
# ===========================================================================


def test_audio_never_fires_when_disarmed(device):
    """The single most important test here. Mode 3, disarmed, beats streaming:
    every valve channel stays 0 for the whole window."""
    device.set_button(mode=3, fireDurationMs=500, cooldownMs=0, endCueMs=0)
    device.audio_disarm()
    with AudioSender(host_ip(device)) as tx:
        tx.grid(bpm=140)
        tx.set_bands(bass=255, level=255)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            assert not any_valve_open(device.get_state()), "fired while disarmed"
            time.sleep(0.03)


def test_audio_never_fires_when_stale(device):
    device.set_button(mode=3, fireDurationMs=500, cooldownMs=0, endCueMs=0)
    device.audio_arm()
    time.sleep(1.0)   # armed but no packets at all
    deadline = time.monotonic() + 1.5
    while time.monotonic() < deadline:
        assert not any_valve_open(device.get_state()), "fired with no audio source"
        time.sleep(0.03)


def test_audio_never_fires_in_non_audio_mode(device):
    """Covers 'device booted in mode 0 with audio armed' — which cannot fire."""
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=0, endCueMs=0)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.grid(bpm=140)
        tx.set_bands(bass=255, level=255)
        deadline = time.monotonic() + 2.5
        while time.monotonic() < deadline:
            assert not any_valve_open(device.get_state()), "audio fired in mode 0"
            time.sleep(0.03)


def test_audio_min_interval_enforced(device):
    """Beats far faster than minGapMs must not produce a shot per beat."""
    device.set_button(mode=3, fireDurationMs=300, cooldownMs=0, endCueMs=0)
    device.set_audio(audMinGapMs=400, audShotMs=100, audDutyPct=50, audLeadMs=0)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        stop = time.monotonic() + 5.0
        samples = []
        while time.monotonic() < stop:
            tx.beat(255)                       # ~20 Hz of beats
            samples.append((time.monotonic(), ch(device.get_state(), TOWER_FIRE_CH[0])))
            time.sleep(0.05)
    edges = rising_edges(samples)
    assert len(edges) <= 5000 / 400 + 2, f"{len(edges)} shots in 5 s with a 400 ms floor"


def test_audio_duty_budget_caps_open_time(device):
    """Two independent views: the sampled wire, and the device's own accounting.

    Polling /api/state perturbs the loop it measures, so neither alone is enough.
    """
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audDutyPct=25, audDutyWinMs=4000, audMaxOpenMs=600, audMinGapMs=250)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)      # hold well above bassOn
        time.sleep(1.0)
        samples = sample_valves(device, 12.0)
    open_frac = sum(1 for _, v in samples if v) / max(1, len(samples))
    assert open_frac <= 0.25 + 0.12, f"measured duty {open_frac:.0%} over the 25% ceiling"
    a = device.audio()
    assert a.get("dutyUsedMs", 0) <= a.get("dutyCapMs", 0) + 100


def test_audio_duty_budget_ignores_cooldown_zero(device):
    """The exact gap this feature opens: with the FSM lockout removed entirely, the
    audio limiter must still hold."""
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audDutyPct=25, audDutyWinMs=4000, audMaxOpenMs=600, audMinGapMs=250)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        time.sleep(1.0)
        samples = sample_valves(device, 12.0)
    open_frac = sum(1 for _, v in samples if v) / max(1, len(samples))
    assert open_frac <= 0.25 + 0.12, f"duty {open_frac:.0%} with cooldownMs=0"


def test_audio_burst_ceiling_is_three_seconds(device):
    """A config whose bucket would allow 30 s must still never open past ~3 s.
    This is the gate a leaky bucket alone does not give you."""
    device.set_button(mode=4, fireDurationMs=30000, cooldownMs=0, endCueMs=0)
    device.set_audio(audDutyPct=50, audDutyWinMs=60000, audMaxOpenMs=3000, audMinGapMs=100)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        samples = sample_valves(device, 12.0)
    longest = max((e - s for s, e in open_runs(samples)), default=0.0)
    assert longest <= 3.0 + 0.25, f"longest continuous open {longest:.2f}s exceeds the 3 s ceiling"


def test_audio_manual_fire_consumes_the_budget(device):
    """Accounting is physical, not per-source: an operator press spends the same
    budget an audio shot would."""
    device.set_button(mode=3, fireDurationMs=2000, cooldownMs=0, endCueMs=0)
    device.set_audio(audDutyPct=25, audDutyWinMs=4000)
    device.press()
    time.sleep(1.5)
    device.release()
    used = device.audio().get("dutyUsedMs", 0)
    assert used > 200, f"manual fire did not charge the budget (dutyUsedMs={used})"


def test_audio_disarm_closes_an_open_valve(device):
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audMaxOpenMs=3000, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) != 0, timeout=4.0)
        device.audio_disarm()
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) == 0, timeout=0.6)


def test_audio_mode_change_closes_an_open_valve(device):
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audMaxOpenMs=3000, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) != 0, timeout=4.0)
        device.set_button(mode=0, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) == 0, timeout=0.8)


def test_audio_stale_mid_shot_closes_the_valve(device):
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audMaxOpenMs=3000, audDutyPct=50, audStaleMs=500)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) != 0, timeout=4.0)
        tx.go_silent()
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) == 0, timeout=1.2)


def test_audio_ota_refused_while_armed(device):
    """safeToStart() refuses before Update.begin(), so nothing is written to flash.

    A stalled main loop cannot run audioFireTick(), so an armed rig could open a
    valve with nothing left to close it.
    """
    device.audio_arm()
    try:
        r = device.post_multipart("/api/update", b"\x00" * 1024)
        assert r.status_code >= 400, "OTA accepted an upload while audio was armed"
        assert "audio" in r.text.lower(), f"refusal reason did not mention audio: {r.text!r}"
    finally:
        device.audio_disarm()


# ===========================================================================
# Prediction
# ===========================================================================


def test_audio_fires_ahead_of_the_beat(device):
    """With a lead configured, the valve must open BEFORE the acoustic instant.

    A reactive implementation fires after it, which is exactly the failure this
    whole design exists to avoid.
    """
    device.set_button(mode=3, fireDurationMs=400, cooldownMs=0, endCueMs=0)
    device.set_audio(audLeadMs=200, audShotMs=150, audMinGapMs=250, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        tx.grid(bpm=120)
        time.sleep(3.0)                      # let the grid become confident
        samples = sample_valves(device, 5.0)
        beats = tx.beat_instants()
    edges = rising_edges(samples)
    assert edges, "no shots fired at all"
    assert beats, "sender produced no beats"

    leads = []
    for e in edges:
        nearest = min(beats, key=lambda b: abs(b - e))
        leads.append(nearest - e)            # positive => fired before the beat
    median = sorted(leads)[len(leads) // 2]
    assert median > 0.05, (
        f"median lead {median * 1000:.0f} ms — the valve is firing at or after the "
        "beat, so prediction is not engaging"
    )


def test_audio_prediction_needs_tempo_confidence(device):
    """Unstable intervals must fall back to reactive triggering rather than firing
    into silence on a guessed grid."""
    device.set_button(mode=3, fireDurationMs=300, cooldownMs=0, endCueMs=0)
    device.set_audio(audLeadMs=250, audShotMs=120, audMinGapMs=250, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        tx.grid(bpm=120, jitter=True)
        time.sleep(3.0)
        assert device.audio()["confident"] is False, (
            "predictor reports confidence on a jittered grid"
        )


def test_audio_prediction_stops_when_grid_dies(device):
    device.set_button(mode=3, fireDurationMs=300, cooldownMs=0, endCueMs=0)
    device.set_audio(audLeadMs=150, audShotMs=120, audMinGapMs=250, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        tx.grid(bpm=120)
        time.sleep(3.0)
        tx.stop_grid()
        time.sleep(1.5)                      # let any in-flight shot end
        samples = sample_valves(device, 3.0)
    assert not rising_edges(samples), "kept firing on a dead beat grid"


def test_audio_predicted_shot_still_obeys_the_limiter(device):
    """Prediction changes WHEN a shot is requested, never WHETHER it is allowed."""
    device.set_button(mode=3, fireDurationMs=300, cooldownMs=0, endCueMs=0)
    device.set_audio(audDutyPct=0)           # 0 = lights only
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.grid(bpm=120)
        tx.set_bands(bass=255, level=255)
        deadline = time.monotonic() + 3.0
        while time.monotonic() < deadline:
            assert not any_valve_open(device.get_state()), "fired with dutyPct=0"
            time.sleep(0.03)


# ===========================================================================
# Behaviour
# ===========================================================================


def test_audio_beat_pop_fires_once_per_beat(device):
    """One shot per beat at 120 BPM.

    shotMs is 250 rather than the 150 default purely for OBSERVABILITY: every sample
    is a ~1.3 KB /api/state round-trip, so the effective polling period is far above
    the nominal 25 ms. A 120 ms pulse is at that floor and gets missed about half the
    time, which reads as "fired every other beat" when the wire is actually correct.
    250 ms at a 500 ms beat is still under 50% duty and is reliably visible.
    """
    device.set_button(mode=3, fireDurationMs=400, cooldownMs=0, endCueMs=0)
    device.set_audio(audShotMs=250, audMinGapMs=250, audDutyPct=50, audLeadMs=0)
    device.audio_arm()
    settle(device)
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        tx.grid(bpm=120)                     # 2 beats/sec
        time.sleep(2.0)
        samples = sample_valves(device, 5.0)
    edges = rising_edges(samples)
    assert 6 <= len(edges) <= 14, f"{len(edges)} shots in 5 s at 120 BPM (expected ~10)"


def test_audio_beat_pop_respects_beat_strength(device):
    device.set_button(mode=3, fireDurationMs=300, cooldownMs=0, endCueMs=0)
    device.set_audio(audBeatMin=200, audShotMs=120, audMinGapMs=250, audDutyPct=50)
    device.audio_arm()
    settle(device)
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        stop = time.monotonic() + 3.0
        samples = []
        while time.monotonic() < stop:
            tx.beat(80)                      # below beatMin
            samples.append((time.monotonic(), ch(device.get_state(), TOWER_FIRE_CH[0])))
            time.sleep(0.1)
    assert not rising_edges(samples), "fired on a beat weaker than beatMin"


def test_audio_sustained_bass_holds_then_releases(device):
    device.set_button(mode=4, fireDurationMs=10000, cooldownMs=0, endCueMs=0)
    device.set_audio(audBassOn=170, audBassOff=140, audMaxOpenMs=2000, audDutyPct=50)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) != 0, timeout=3.0)
        tx.set_bands(bass=100)               # below bassOff
        device.wait_until(lambda s: ch(s, TOWER_FIRE_CH[0]) == 0, timeout=1.0)


def test_audio_drop_mode_ignores_ordinary_beats(device):
    device.set_button(mode=5, fireDurationMs=500, cooldownMs=0, endCueMs=0)
    device.set_audio(audDropGapMs=2000, audDropShotMs=300, audDutyPct=50, audDropMin=200)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=60, level=60)
        tx.grid(bpm=120)
        time.sleep(2.0)
        samples = sample_valves(device, 2.0)
        assert not rising_edges(samples), "drop mode fired on an ordinary beat"

        # Sample immediately — dropShotMs is 300 ms, so sleeping first would miss the
        # entire shot and report zero edges.
        tx.bighit()
        samples2 = sample_valves(device, 1.2)
        assert len(rising_edges(samples2)) == 1, "big hit did not fire exactly once"

        tx.bighit()                          # inside dropGapMs — must be ignored
        samples3 = sample_valves(device, 1.2)
    assert not rising_edges(samples3), "dropGapMs did not gate the second hit"


def test_audio_machine_gun_locks_to_the_beat(device):
    """Contiguous open groups should track beat count.

    A free-running phase produces a group count set by the pulse period instead,
    which is what distinguishes mode 6 from mode 2.
    """
    device.set_button(mode=6, fireDurationMs=10000, cooldownMs=0, endCueMs=0,
                      machineGunBurstMs=150)
    device.set_audio(audBassOn=100, audMaxOpenMs=3000, audDutyPct=50, audLeadMs=0)
    device.audio_arm()
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, level=255)
        tx.grid(bpm=120)
        time.sleep(2.5)
        samples = sample_valves(device, 4.0)
    runs = open_runs(samples)
    assert runs, "mode 6 never opened a valve"
    # 4 s at 120 BPM is ~8 beats; allow generous slack for the 20 Hz sampling floor.
    assert 4 <= len(runs) <= 13, f"{len(runs)} pulse groups in 4 s (expected ~8)"


# ===========================================================================
# Lights
# ===========================================================================


def test_audio_lights_react_when_fresh_but_disarmed(device):
    """Lights follow freshness alone; fire additionally needs armed + an audio mode."""
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCueMs=0)
    device.set_all_towers(theme="green", brightness=40)
    device.set_audio(audLightMode=1, audLightDepth=255)
    device.audio_disarm()
    uplight = TOWER_FIRE_CH[0] + 1           # uplight block starts one past the valve
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, mid=255, treble=255, level=255)
        seen_light = False
        deadline = time.monotonic() + 4.0
        while time.monotonic() < deadline:
            s = device.get_state()
            assert not any_valve_open(s), "lights test opened a valve"
            if any(ch(s, uplight + n) > 40 for n in range(3)):
                seen_light = True
            time.sleep(0.05)
    assert seen_light, "uplight never responded to audio"


def test_audio_light_glow_survives_the_gradient_blank(device):
    """green/blue/fire are fully dark for 3200 ms of every 4000 ms cycle.

    A multiplicative implementation is invisible for that whole stretch and fails
    this immediately; the additive max() composition lights the blank phase.
    """
    device.set_button(mode=0, fireDurationMs=500, cooldownMs=2000, endCueMs=0)
    device.set_all_towers(theme="green", brightness=128)
    device.set_audio(audLightMode=1, audLightDepth=255)
    uplight = TOWER_FIRE_CH[0] + 1
    with AudioSender(host_ip(device)) as tx:
        tx.set_bands(bass=255, mid=255, treble=255, level=255)
        time.sleep(0.5)
        dark = 0
        deadline = time.monotonic() + 4.5    # more than one full theme cycle
        while time.monotonic() < deadline:
            s = device.get_state()
            if all(ch(s, uplight + n) == 0 for n in range(3)):
                dark += 1
            time.sleep(0.05)
    assert dark == 0, f"uplight was fully dark on {dark} samples despite constant audio"
