# Spec: OTA firmware update over WiFi

## Context

USB flashing on this board is slow and unreliable. Measured on a real 1.1 MB
upload:

| | |
|---|---|
| Wall clock | **461 s** (7m41s) |
| Actual data transfer | **8.8 s** — every 64 KB block writes in ~0.5 s |
| Retries burned | 24 |
| Efficiency | **~2%** |

That flash then failed outright on the final block and needed a physical power
cycle to recover. Everything expensive about it — the USB-Serial/JTAG handshake,
the per-block reset, the macOS serial stack, the errata retries — is overhead
around 8.8 seconds of real work.

None of it is necessary. The partition table already carries two OTA slots, and
the device already runs a web server on its own AP:

```
nvs         data  nvs      0x9000      20K
otadata     data  ota       0xe000       8K
app0        app   ota_0    0x10000    3264K   ← firmware is 1123K
app1        app   ota_1   0x340000    3264K
spiffs      data  spiffs  0x670000    1536K
coredump    data  coredump 0x7f0000     64K
```

So the same binary can be pushed over WiFi in seconds, touching none of that
hardware. **OTA is the primary upload path. USB stays as the recovery path** for
a device that is bricked, has no working WiFi, or is running firmware without
the OTA endpoint.

Related: [spec-upload.md](spec-upload.md) (the USB path and what was corrected
about it), [spec-flash-recovery-failover.md](spec-flash-recovery-failover.md).

---

## ⚠️ Safety: an upload stops DMX

This is the constraint the whole design is shaped around.

An upload occupies the main loop for its entire duration. While it runs there
are **no DMX frames**, and DMX fixtures hold their last commanded value when the
signal stops. A valve open at that moment stays open, with the FSM frozen and
nothing left running to close it.

Two mitigations, both mandatory:

1. **Refuse unless the rig is provably idle.** `safeToStart()` in `ota.cpp`
   rejects the upload unless `fsmState == FSM_IDLE`, `!purgeActive()` and
   `!morseActive()`. The rejection happens at `UPLOAD_FILE_START`, before
   `Update.begin()` is ever called, and the rest of the body is discarded.
2. **Drive every valve to 0 on the wire first.** `forceEverythingClosed()` zeroes
   all four towers plus Confluence and then emits several real frames, paced past
   the TX-drain guard in `dmxUpdate()`. Zeroing the shadow buffer is not enough —
   the bytes have to reach the fixtures before the loop stalls.

Because the FSM is frozen during the upload, no fire can *start* mid-upload
either: the button is never sampled.

`scripts/ota.sh` and the web UI both check idleness client-side too, so a
rejected upload fails fast with a clear message instead of a 500 partway
through. The device check is the authoritative one.

---

## Firmware

New module `ota.h` / `ota.cpp`, registered from `webSetup()` **after** all other
routes so its handler wins over `onNotFound`:

```cpp
otaRegister(server);
```

`POST /api/update` — multipart upload of `Test_Button_DMX.ino.bin`, handled with
`Update.h` and `WebServer`'s two-callback upload form: a streaming body callback
plus a completion callback that responds and reboots.

- `Update.begin(UPDATE_SIZE_UNKNOWN)` — size is not known up front with chunked
  multipart. `Update` rolls into whichever OTA slot is not currently running and
  `Update.end(true)` sets the boot slot in `otadata`.
- Progress is logged once per ~128 KB — enough to watch, not enough to flood.
- On success the device responds `{"ok":true,"bytes":N}`, waits 200 ms for the
  response to leave the socket, then `ESP.restart()`.
- On failure it responds `{"ok":false,"error":"..."}` with the reason.

### `/api/state`

```json
"ota": { "inProgress": false, "lastError": "" }
```

`lastError` survives the response so the UI can explain a rejection.

---

## Client: `scripts/ota.sh`

```bash
scripts/ota.sh                    # compile + push
scripts/ota.sh --no-compile       # push the existing build
DMXFIRE_HOST=http://10.0.0.42 scripts/ota.sh
```

Compiles, checks `/api/state` for idleness, POSTs the binary, then polls until
the device returns with a fresh `boot_id` and a small `uptime_ms`.

**A dropped connection is not a failure.** The device reboots the instant it
accepts the image, which frequently kills the response in flight. The script
treats an empty/`000` HTTP code as inconclusive and verifies by polling rather
than reporting a false error.

---

## Web UI

New **Firmware** tab: file picker, upload button, progress bar, status line.

Uses `XMLHttpRequest`, not `fetch` — `fetch` exposes no upload-progress events,
and a 1.1 MB push over the device AP is slow enough to need a bar. Same
reboot-races-the-response handling as the script: an `error` event at ≥99%
is reported as success.

Source of truth is `tools/web-preview/index.html`, mirrored into `web.cpp`'s
`buildPage()` via `/web-sync`. `buildPage()`'s `s.reserve()` was raised from
32000 to 36000 to cover the added markup and script.

`tools/web-preview/server.py` mocks `/api/update` with the same idle gating so
the tab is testable in the preview. Note it routes `/api/update` **before**
`_read_form()`: that helper reads the whole body and utf-8 decodes it, which
throws on a binary image and kills the request with no response at all.

---

## Persistence

None. OTA adds no NVS keys. Config in `nvs` is untouched by an OTA — settings
survive an update, unlike `scripts/flash.sh --erase`.

---

## Verification status

**The firmware OTA path has not been exercised on hardware** — it was written
with no device available. What has been verified:

- Compiles clean (1159340 bytes, 34% of program storage).
- The HTTP contract, idle gating, and binary-body handling are verified end to
  end against the mock server: accepted when IDLE, refused with
  `{"ok":false,"error":"FSM is FIRE_ACTIVE, must be IDLE"}` mid-fire.
- `scripts/ota.sh`'s `/api/state` field extraction is verified against a real
  state payload.

Untested on hardware: `Update.begin/write/end`, the OTA slot roll, the reboot,
and real WiFi throughput. **First OTA should be done with USB recovery
available**, since a bad image means a USB reflash.

---

## Non-goals

- **No OTA over the internet / no pull-based updates.** Push-only, over the
  device's own AP or a LAN it has joined.
- **No authentication.** The AP is the security boundary, matching every other
  endpoint on this device. Anyone who can reach `/set` can already open valves;
  OTA is not a new exposure. If the AP ever gains a password or the device joins
  an untrusted network, this needs revisiting.
- **No rollback UI.** `Update.end(true)` sets the boot slot; the previous image
  stays in the other slot but nothing exposes a "boot the old one" control.
  Recovery from a bad image is a USB reflash.
- **No firmware version reporting.** `boot_id` distinguishes reboots, not
  builds. A version string would be a sensible follow-up.
- **No OTA while firing.** Deliberate — see the safety section.
- **USB is not removed.** `scripts/flash.sh` remains the recovery path and the
  only way to flash bootloader/partition-table changes, which OTA cannot touch.
