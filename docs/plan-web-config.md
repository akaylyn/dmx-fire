# Plan: Web Configuration UI

## Goal

Run a small HTTP server on the M5AtomS3 Lite so that a user can connect to it via a browser and configure the active DMX palette using a dropdown.

---

## Architecture decisions

### WiFi mode: Access Point (AP)
The device will broadcast its own WiFi network (`dmx-fire` / no password by default). The user connects to that network, navigates to `192.168.4.1`, and gets the config page. This requires no existing network credentials and is the natural fit for a "configure this device" workflow.

### Web server: built-in `WebServer`
The ESP32 Arduino core ships `WebServer.h` — synchronous, simple, no extra library. Sufficient for a low-traffic config page.

### No SPIFFS / no file system
The HTML page is small enough to serve as a raw string from program memory (`PROGMEM`). Keeps the build simple.

---

## Code changes

### 1. Globals
Promote `currPal` and `currBright` from `static` locals in `loop()` to proper globals, so the web handler can write to them.

```cpp
CRGBPalette256 currPal   = electricGreenFirePal;
byte           currBright = 16;
```

### 2. WiFi + server setup (in `setup()`)
```
WiFi.softAP("dmx-fire");
server.on("/", handleRoot);       // serve the HTML page
server.on("/set", handleSet);     // handle form POST
server.begin();
```

### 3. Server tick (in `loop()`)
```
server.handleClient();
```
Placed at the top of `loop()`, before any `EVERY_N_MILLISECONDS` blocks.

### 4. `/` — root handler
Serves an HTML page with:
- A `<select>` dropdown listing the three palettes
- Current selection pre-selected
- A submit button POSTing to `/set`

### 5. `/set` — set handler
Reads the `palette` field from the POST body, maps it to the right `CRGBPalette256`, updates `currPal`. Redirects back to `/`.

Palette name → value map:

| Dropdown label      | POST value    |
|---------------------|---------------|
| Green fire (default)| `green`       |
| Blue fire           | `blue`        |
| Natural fire        | `fire`        |

---

## What is NOT changing

- Button behaviour is unchanged — press still overrides to blue fire at full brightness, release restores the web-configured idle palette.
- DMX output loop is unchanged.
- No persistence across reboots (can add `Preferences` later).

---

## Decisions

1. **AP password** — `dmxfire1` (simple password for testing; make configurable in a future release).
2. **Button override vs web config** — button press temporarily overrides to blue fire at full brightness; release restores the web-configured idle palette. Button release behaviour will be made configurable in a future release.
3. **Brightness control** — in scope. Add a range slider (`0–255`) to the config page. POSTed alongside `palette` to `/set`. Stored in `currBright` global.
