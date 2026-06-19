# Spec: Captive-portal dismiss ("Open in real browser")

## Context

Joining the device's WiFi AP triggers the OS captive-portal popup, which loads `192.168.4.1` inside a constrained webview. That webview is a trap on iOS/Android: external links don't open well, switching apps can drop the WiFi association, and iOS eventually disconnects a "no-internet" network. Operators want to dismiss the popup and reopen the UI in real Safari/Chrome without losing WiFi.

This spec adds a button at the bottom of the Test Fire tab that (a) tells the firmware to stop returning captive-portal redirects to OS probe URLs, so the OS's next connectivity check succeeds and the popup auto-closes, and (b) shows instructions for opening a real browser.

---

## Mechanism

A RAM-only flag `captivePortalActive` (file-scope in `web.cpp`) defaults to `true` on every boot. While `true`, the DNS hijack + HTTP redirects behave as before, so first-time AP join still pops the captive UI. `POST /api/captive/dismiss` sets it `false`.

When `false`, the OS connectivity-probe URLs return their platform's "success" response instead of a redirect, so the OS marks the network as having internet and closes the popup. The flag is **never persisted** — a reboot restores the captive flow so first-time setup always works.

### Probe URL handlers (registered in `webSetup()` before `onNotFound`)

| Path | Platform | Active (redirect) | Dismissed |
|---|---|---|---|
| `/hotspot-detect.html`, `/library/test/success.html` | iOS | `302 → /` | `200` `<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>` |
| `/generate_204`, `/gen_204` | Android | `302 → /` | `204 No Content` |
| `/ncsi.txt` | Windows | `302 → /` | `200` `Microsoft NCSI` |
| `/connecttest.txt` | Windows | `302 → /` | `200` `Microsoft Connect Test` |

`onNotFound` mirrors this: `302 → http://192.168.4.1/` while active; `200 Success` once dismissed (so a stray probe also accepts the network rather than re-triggering the popup).

---

## HTTP endpoint

| Method | Path | Effect | Response |
|---|---|---|---|
| POST | `/api/captive/dismiss` | `captivePortalActive = false` | `204 No Content` |

Logged at INFO: `[WEB] POST /api/captive/dismiss`.

The mock server (`tools/web-preview/server.py`) returns `204` for the same path so the preview flow works without errors.

---

## Web UI (`tools/web-preview/index.html` → `web.cpp`)

At the bottom of the Test Fire tab, below the fire button:

- A subtle secondary `#openInBrowserBtn` ("Open in real browser") — not red, doesn't compete with the fire button.
- A modal `#browserEscapeModal` (hidden by default) with three numbered steps (close the captive portal per-OS, open Safari/Chrome, visit `http://192.168.4.1`) and a "Got it" close button.

JS: clicking the button `fetch('/api/captive/dismiss', {method:'POST'})` then shows the modal on either success or failure (`.then(show, show)`) — the instructions are useful even if the POST fails. Close button and backdrop click hide the modal.

---

## Persistence

None. `captivePortalActive` is RAM-only and resets to `true` every boot. Persisting it would break first-time AP setup, so it is intentionally not stored in NVS.

---

## Non-goals

- **Programmatically closing the captive popup.** Not possible from inside the webview; we only make the next OS probe succeed so the OS closes it.
- **A persistent "always behave as a normal AP" mode.** Out of scope; the captive flow is wanted for first contact.
- **Changing the DNS hijack or AP SSID/password.** Unchanged.
