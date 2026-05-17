# Spec: Morse Code Fire Playback

## Context

The button modes ([Fireball, Party, Machine Gun](spec-machine-gun.md)) all produce uniform output — they either hold the solenoid open or pulse it on a fixed period. None can encode arbitrary information.

This spec adds Morse code playback: type a text message into the web UI, hit "Fire in Morse," and the propane solenoid pulses out international Morse code for that text. Useful for performance moments where the fire becomes a message rather than just an effect.

While a Morse playback is running, it takes priority over the FSM-driven fire output — Test Fire / physical button presses still update the FSM but do not reach CH4.

---

## Morse encoding

International Morse code, A–Z and 0–9. Unsupported characters (punctuation, accents) are silently skipped. Spaces in the input become inter-word gaps.

Timing in "units" (one unit = `morseUnitMs`, default 150 ms, configurable 50–500 ms):

| Element | Duration |
|---|---|
| Dot | 1 unit ON |
| Dash | 3 units ON |
| Gap between elements within a letter | 1 unit OFF |
| Gap between letters | 3 units OFF |
| Gap between words | 7 units OFF |

### Encoding representation

`morseStart()` converts the input text into a sequence string where each character is one Morse unit: `'1'` = fire ON, `'0'` = fire OFF. Example for `"SOS"`:

```
S = . . .       → 10101
letter gap      → 000
O = - - -       → 11101110111
letter gap      → 000
S = . . .       → 10101
```

Final sequence: `"10101" + "000" + "11101110111" + "000" + "10101"` (35 units = 5250 ms at 150 ms/unit).

`morseTick()` returns `confluenceConfig.fireLevel` when the current unit (computed from `millis() - startMs`) is `'1'`, else 0. When the index runs past the end of the sequence, playback ends and the function returns 0.

---

## Files

- **`morse.h`** — public API: `morseStart(String)`, `morseStop()`, `morseActive()`, `morseTick()`, plus `extern uint16_t morseUnitMs`.
- **`morse.cpp`** — A–Z + 0–9 lookup table, text→sequence encoder with proper gap handling, time-based tick.
- **`Test_Button_DMX.ino`** — in the 50 Hz DMX tick, `morseActive()` is checked before the FSM:

  ```c
  if (morseActive()) {
    cfLevel = morseTick();
  } else if (fsmState == FSM_FIRE_ACTIVE) {
    /* existing FSM-driven cfLevel */
  }
  ```

---

## Web UI (`web.cpp`)

New **Morse Code** fieldset, placed directly below Button Config so it's near the top of the page:

- Text input (`<input id="morseText" maxlength="80">`), placeholder `HELLO WORLD`.
- **Unit (ms)** slider (`name="morseUnitMs"`, 50–500 ms, step 10, default 150).
- **Fire in Morse** button (`id="morseGo"`) — POSTs `text` and `unitMs` to `/api/morse`.
- **Stop** button (`id="morseStop"`) — POSTs to `/api/morse/stop`.

The fieldset has no `<form>`, so values are not auto-saved (Morse is intentionally a one-shot trigger).

---

## HTTP endpoints

| Method | Path | Body params | Effect | Response |
|---|---|---|---|---|
| POST | `/api/morse` | `text` (required), `unitMs` (optional, 50–2000) | Updates `morseUnitMs` if provided; calls `morseStart(text)` | `200` `OK` on success; `400` `no codable characters` if input has no letters/digits |
| POST | `/api/morse/stop` | — | Calls `morseStop()` (no-op if not active) | `200` empty |

Logs at INFO level: `[MORSE] starting: <units> units * <ms> ms = <total> ms total` on start, `[MORSE] playback complete` when the sequence runs out, `[MORSE] stopped` on explicit stop.

---

## Persistence

`morseUnitMs` is **not** persisted to NVS — it resets to 150 ms each boot. The slider value can be tuned per-session but doesn't survive a reboot. Adding NVS persistence is a 5-line change but not currently warranted; Morse usage is rare enough that defaults are fine.

---

## Interaction with other features

- **Button-driven fire:** Physical button presses and `POST /api/button/press` still advance the FSM through `FIRE_ACTIVE`, but CH4 stays muted by the Morse override. When playback ends, normal FSM behaviour resumes.
- **Tower lights:** Unaffected. Morse only drives CH4 (Confluence solenoid). Towers continue their normal idle/fire-state behaviour based on the FSM.
- **Cooldown:** Morse playback ignores `cooldownMs` — it has its own timing.

---

## Non-goals

- **Audible/visual indicator on the Atom LED.** Could blink the onboard LED in time with the sequence, but it isn't urgently needed and would complicate the LED-status logic in `Test_Button_DMX.ino`.
- **Punctuation, accented characters, special prosigns.** Skipped silently. Adding them is a table-entry change if ever needed.
- **Queueing / playlist of messages.** One message at a time. Calling `morseStart()` while a sequence is playing replaces it.
