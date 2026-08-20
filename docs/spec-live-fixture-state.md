# Spec: Live fixture state in the web UI

## Context

The config forms in `buildPage()` are **server-rendered once, at page load**:
`connectedCheck()` bakes `checked` into the HTML, `rangeSlider()` bakes `value=`. Nothing
refreshed them afterwards. So any change made by something other than that page — the REST
API, a pytest run, another phone on the AP — left the form showing stale values, silently.

Two consequences, both real:

1. **The page could lie about a fixture.** It could show *Connected* ticked while NVS said
   otherwise. A disconnected tower is skipped entirely in the frame loop
   (`Test_Button_DMX.ino:209`), so its channels are never written and sit at zero — on the
   wire, indistinguishable from a dead decoder.
2. **Saving a stale form wrote the stale values back**, because each form posts *every*
   field. Adjusting one slider could silently revert everything else on that fixture.

This cost a field session: Tower 1's accumulator was declared faulty and **physically
replaced**, and its cable pin-tested, before anyone checked that `connected` was `false`
(notes.md Session 5).

---

## Technical details

A polling block in the page script keeps the Tower and Confluence forms honest.

**Sync.** Every second, while a tab holding a readout is visible, `GET /api/state` and
write each fixture's live config back into its form — checkbox, theme select, and the
range inputs plus their `<span class='val'>` mirrors.

**Edit protection.** Blind overwriting would fight the operator mid-drag. Any `input` or
`change` on a form sets `dataset.dirty`; a dirty form is **not** synced and instead shows
*"Form has unsaved edits — not syncing from the device."* Submitting clears the flag and
syncing resumes. A focused field is never overwritten even on a clean form.

**Live readout.** Each fixture gets a `div.live` under its address line showing the bytes
currently composed for its block — `on air   decoder 96/96/96/0   uplight 128/128/128/0` —
computed from `dmx.ch` with the same `base = 4 + i * 15` stride as `towers.cpp`.

**Loud failure banners**, because every one of these is silent in normal operation:

| Condition | Message |
|---|---|
| `connected: false` | DISCONNECTED — names the exact channel range that stays at 0, and the fix |
| `brightness: 0` | strips stay black (the uplight still lights during fire, via `applyFireLook()`) |
| `flameLevel: 0` | that valve channel never opens |
| `flameLevel < 128` | on/off valve, not a dimmer — may fail to energise or chatter the coil |
| Confluence `connected: false` | `confluenceWrite()` skipped, CH1 never fires |
| `button.fireDurationMs < 50` | shorter than one DMX frame — can only reach the wire as a single frame, too brief to light |
| `dmx.quiet` | drops "on air" entirely — *"composed but not sent"* |

**Polling is visibility-gated.** Each request is handled synchronously inside `webTick()`,
so background polling would add DMX frame jitter for nothing — the same rule the Audio tab
already follows.

**Related correction.** `flameLevel` / `fireLevel` are **not** proportional flame controls.
The solenoid is an on/off valve; the byte only has to clear the decoder's turn-on threshold
to energise the coil. Flame *size* is gas pressure and orifice, not DMX. The old
`towers.h` comment ("0=off, 255=full open") said otherwise and has been rewritten.

---

## Web UI changes

Source of truth is `tools/web-preview/index.html`, ported to `buildPage()` via `/web-sync`
with the JS minified to match the file's inline style. New `.live` CSS; `data-live`
elements on each tower panel, the Confluence panel, and the Bus handover fieldset.

`tools/web-preview/server.py` now derives `dmx.ch` from the mock's tower config
(`build_dmx_frame()`), reproducing the disconnected-tower-reads-zero behaviour so the
preview exercises the banner rather than always showing zeros.

## Persistence

None. Read-only display plus form hydration; nothing new is stored.

---

## Non-goals

- **Not a DMX monitor.** It shows the controller's own composed frame, not what is on the
  wire. Verifying the wire is the Enttec's job via `tools/dmx-tester/`.
- **No live push.** Polling at 1 Hz, gated on tab visibility; no WebSocket or SSE on a
  synchronous single-threaded web server.
- **Does not auto-correct config.** It reports and hydrates; changing a value is still an
  explicit Save. Nothing here writes to the device on its own.
