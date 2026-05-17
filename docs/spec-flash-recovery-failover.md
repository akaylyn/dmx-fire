# Spec: Flash Recovery Failover

## Context

`scripts/flash.sh` writes the app binary one 64 KB block per esptool connection with up to 12 retries per block (see [spec-upload.md](spec-upload.md)). When a block can't be written even after 12 attempts, the device is typically in one of two states the CLI can't recover from:

1. **USB-CDC stack desynced** — the previous firmware corrupted the chip's internal USB CDC engine. macOS still enumerates a `cu.usbmodem*` port but the esptool reset/handshake never completes.
2. **Boot-loop too tight** — the partial app crashes fast enough that no clean download-mode window opens.

The cure in both cases is the ROM-bootloader override: hold the side button, replug USB, release. This is silicon-level and tool-agnostic. Once the chip is in ROM bootloader mode, esptool-js over Web Serial in Chrome/Edge often succeeds because the browser uses a different OS USB driver path than the CLI esptool — and forces the operator through the button-hold reset.

This spec covers the automatic failover from `scripts/flash.sh` into the browser-based recovery tool when block writes have exhausted retries.

---

## Failover trigger

Inside `flash.sh`, both `do_flash` call sites:

```bash
do_flash ... || { recovery_failover; exit 2; }
```

`do_flash` already retries 12 times per esptool invocation. By the time it returns non-zero, the CLI has run for ~50 s on a single block without success — there is no point retrying further; hand off to the browser.

---

## `recovery_failover` behavior

1. Print a red banner stating the CLI flash failed and recovery is being launched.
2. Print the four binary paths the operator must drop into the browser tool, paired with their flash offsets:

   ```
   0x00000  $BIN_BOOT
   0x08000  $BIN_PART
   0x0e000  $BOOT_APP0
   0x10000  $BIN_APP
   ```

3. Print the manual button-hold steps (hold side button → replug USB → release → Connect in browser).
4. Probe internet reachability: `curl -fsSL -m 3 -o /dev/null https://espressif.github.io/esptool-js/`.
5. **If reachable:** `open https://espressif.github.io/esptool-js/` (the upstream hosted demo).
6. **If unreachable:** spawn `python3 -m http.server 8765` rooted at `tools/recovery/` in the background, save its PID, and `open http://localhost:8765/`. The script then `wait`s on the server PID so the user can complete the flash before the terminal exits. The `EXIT` trap kills the server.

The 3 s curl timeout is short enough that an offline operator doesn't sit through a long DNS stall, but long enough to tolerate a slow uplink.

---

## Local recovery page (`tools/recovery/`)

Offline fallback. Two files, both static, served either by `python3 -m http.server` or any equivalent local HTTP server:

- **`esptool.js`** — esm.sh single-file bundle of `esptool-js@0.6.0` (~97 KB ESM). Self-contained; no further imports.
- **`index.html`** — minimal Web Serial UI. Loaded via `<script type="module">`. Sections:
  1. Download-mode instructions (button-hold steps).
  2. `Connect` / `Disconnect` buttons. `Connect` calls `navigator.serial.requestPort()`, wraps it in `esptool-js` `Transport`, instantiates `ESPLoader` at 115200 baud, and runs `loader.main()` to detect the chip.
  3. Four `<input type="file">` slots prelabeled with `0x00000` / `0x08000` / `0x0e000` / `0x10000`.
  4. `Erase flash` — calls `loader.eraseFlash()`.
  5. `Program all` — reads each file as a binary string (chunked to avoid stack overflow on the 1.1 MB app), calls `loader.writeFlash({ fileArray, flashSize: "keep", flashMode: "dio", flashFreq: "40m", compress: true })`. Progress is reported in the connection-status text.
  6. Live log pane (terminal-style green-on-black) that mirrors esptool-js output via a `terminal` adapter.

The page intentionally does **not** auto-load firmware paths from URL params — the operator pastes file paths from the terminal output. This keeps the page free of repo-specific assumptions and matches the upstream esptool-js UX, so the operator's workflow is identical whether they hit the hosted tool or the local fallback.

---

## Why not bypass the CLI flow entirely

The browser tool is **strictly a fallback**. The CLI block-by-block write remains the primary path because:

- esptool-js writes via the same chip-level protocol over the same USB-JTAG interface, so the ESP32-S3 rev 0.2 64 KB block-erase errata still applies. The upstream hosted demo writes everything in a single session, which can trigger the bug.
- The CLI flow runs unattended; the browser flow requires the operator to click through Connect → file picker × 4 → Program.

The failover catches the cases where the CLI's macOS USB driver path gets wedged and a different OS driver path (Chrome's Web Serial) succeeds — and where the act of switching tools forces the operator through the button-hold reset that is the actual cure.

---

## Files

- `tests/visual/scripts/flash.sh` — `recovery_failover()` function plus `RECOVERY_SERVER_PID` cleanup in the existing `EXIT` trap.
- `tools/recovery/index.html` — local recovery UI.
- `tools/recovery/esptool.js` — bundled esptool-js@0.6.0 ESM.

---

## Non-goals

- **Auto-populating firmware files into the hosted tool.** The hosted page can't accept files via URL params, and changing that is upstream territory.
- **Replacing the CLI flash with the browser flow.** See "Why not bypass" above.
- **Hosting our own copy on GitHub Pages.** A repo-local copy under `tools/recovery/` is enough for the offline case; another hosted endpoint adds maintenance with no benefit over the upstream demo.
- **Auto-detecting which `cu.usbmodem*` port is in download mode.** The browser's port-picker dialog is the user's choice point; trying to pre-select wouldn't survive a USB re-enumeration anyway.
