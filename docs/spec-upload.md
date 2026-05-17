# Spec: Firmware Upload (`scripts/flash.sh` / `/upload`)

## Context

The M5AtomS3 (ESP32-S3 rev 0.2) presents two upload challenges that the naive `esptool write_flash` approach does not solve:

1. **USB-JTAG flaky during large flash erases.** The device's USB-Serial/JTAG controller drops the host connection partway through a single-session write of the 1.1 MB app binary. This has been a recurring problem across many sessions, not a one-time event. The root cause is undocumented by Espressif; the symptom (random crashes mid-flash) is open bug [ESPTOOL-608](https://github.com/espressif/esptool/issues/832).
2. **Boot-loop reflash.** Once the app partition is partially corrupted, the bootloader cycles fast enough that opening a clean download-mode session is timing-dependent.

`scripts/flash.sh` (and the `/upload` skill that wraps it) solves both by writing the app **one 64 KB block per esptool connection**, with hash verification per block and retries on failure. A full flash takes ~5 minutes but completes deterministically.

---

## Operator commands

```bash
scripts/flash.sh            # compile (if needed) + flash
scripts/flash.sh --erase    # also wipes NVS partition (resets stored config to defaults)
```

The `/upload` Claude Code skill invokes `scripts/flash.sh` directly.

---

## Pre-flight checklist

Before running an upload, verify:

- [ ] **Bluetooth is OFF** on the host Mac. macOS Bluetooth enumeration interferes with the USB-Serial/JTAG bus mid-flash. Disable it from the menu bar or System Settings.
- [ ] USB cable carries data (not charge-only). Charge-only cables light up the device LED but never establish a serial connection.
- [ ] The device is plugged in and `ls /dev/cu.usbmodem*` shows a port.
- [ ] No other flash instance is running. The script's lockfile blocks concurrent flashes — do not `pkill` or bypass it.

---

## Algorithm

### Step 0 — Compile (skipped if up to date)

`arduino-cli compile -e --fqbn m5stack:esp32:m5stack_atoms3 Test_Button_DMX`. The script compares mtimes of every `.ino`/`.cpp`/`.h` in the sketch directory against the built `.bin` and skips compilation when nothing changed.

### Step 1 — Small files in one session

Bootloader (~20 KB at `0x0`), partition table (~3 KB at `0x8000`), and OTA-data (~8 KB at `0xe000`) are written together in one `esptool write-flash` call. These are small enough that the single-session USB-JTAG dropout doesn't trigger.

### Step 2 — App binary, one 64 KB block per esptool connection

The 1.1 MB app at `0x10000–0x121950` is split into 17 × 64 KB + 1 × 6800 B tail blocks. For each block:

1. Find current `/dev/cu.usbmodem*` port (it can change between blocks if the device resets).
2. Connect with `esptool --before usb-reset --after watchdog-reset --connect-attempts 10`.
3. Write that one block with `--flash-mode dio --flash-freq 40m --flash-size keep`.
4. Confirm success when the output contains `Hash of data verified` (esptool's teardown error after watchdog-reset is expected — the chip resets before esptool finishes its goodbye write).
5. On failure: up to 12 attempts per block, with a 4 s settle between attempts.

The watchdog-reset between blocks forces a clean USB re-enumeration, so each connection has a fresh boot-loop-window timing rather than chaining state from the previous block.

### Step 3 — Settle

After the last block, the script waits 8 s for the device to boot, then prints `SAFE TO UNPLUG`.

---

## Why each flag matters

| Flag | Purpose |
|---|---|
| `--before usb-reset` | Triggers download mode via the USB-Serial/JTAG control endpoint. `default-reset` (RTS/DTR) doesn't work on this board — there is no physical UART. |
| `--after watchdog-reset` | Forces a full system reset (not just a core reset) so the bootstrap pins are re-sampled and the chip exits download mode cleanly. |
| `--flash-mode dio` | The bootloader runs in DIO mode (app switches to QIO at runtime). Matches the binary header. |
| `--flash-freq 40m` | Conservative SPI clock during flash writes; the chip's default is 80 MHz but 40 MHz reduces signal-integrity edge cases on flaky cables. The header is patched so the bootloader uses 40 MHz at runtime too. |
| `--flash-size keep` | Reads the size from the binary header (8 MB on this board). |
| `--connect-attempts 10` | Tolerate a few missed connection windows during the boot-loop. |
| `-b 115200` | The reliable baud for USB-Serial/JTAG on this chip. Higher speeds are not faster in practice (USB CDC is the bottleneck, not the baud setting) and degrade connection stability. |

---

## Firmware-side causes of USB-JTAG failure

The chip emulates its USB-Serial/JTAG interface in software running on the same cores as the application — there is no dedicated UART bridge. So a misbehaving sketch can wedge the upload path. Avoid the following in firmware to keep the device flashable without manual ROM-bootloader recovery:

- **Boot-time WDT reset loops.** Any code path that crashes or triggers the watchdog before `loop()` settles will cycle fast enough that the USB CDC engine never finishes enumeration. Defer heavy initialization, and never block in `setup()` waiting on hardware that may be absent.
- **Deep sleep without keeping the USB peripheral powered.** Entering `esp_deep_sleep_start()` (or any RTC-only sleep mode) tears down the USB CDC engine; the host sees the port disappear and the next upload attempt can't reset the chip. If sleep is needed, gate it behind a runtime check that keeps USB up while a host is connected.
- **Disabling "USB CDC On Boot".** This Arduino-IDE flag (and its underlying `CONFIG_TINYUSB_CDC_ENABLED`) is what gives us a serial port at all on this board. Don't toggle it off in code or `boards.txt` overrides.

If any of these slip in, the symptom is the same: `scripts/flash.sh` can't connect even on a fresh `--erase`, and the fix is the manual button-hold ROM-bootloader recovery described below.

---

## Recovery paths

### Block-write keeps failing (12 retries exhausted)

`scripts/flash.sh` auto-launches the browser-based recovery tool when any block exhausts its retries — see [spec-flash-recovery-failover.md](spec-flash-recovery-failover.md). In short, the terminal prints the four `.bin` paths and offsets, then opens either <https://espressif.github.io/esptool-js/> (if online) or a local copy served from `tools/recovery/` at `http://localhost:8765/` (if offline). Operator workflow in either case:

1. Hold the side button on the M5AtomS3.
2. Unplug + replug USB while holding; release the button (chip is now in ROM bootloader mode).
3. In the browser: **Connect** → select the `cu.usbmodem*` port.
4. Load each `.bin` at its printed offset and click **Program** (or **Erase Flash** first if you want a full wipe before retrying the CLI).

Web Serial uses a different OS USB driver path than the CLI esptool, so it often succeeds when the CLI gets wedged.

### NVS reset only (config defaults)

`scripts/flash.sh --erase` clears `0x9000`–`0xE000` (NVS) before the regular block-by-block firmware write. Tower config, button config, and palette selections reset to defaults.

### Serial monitor shows binary garbage

That's the ESP32 ROM bootloader's "waiting for download" output printed when no valid app is present — it contains binary register dumps interleaved with ASCII. It disappears the moment the firmware runs. Set the monitor to 115200 baud.

---

## Non-goals

- **OTA updates.** Not implemented yet; would eliminate USB entirely for normal iteration. Until then, `scripts/flash.sh` is the only path.
- **Faster flashing.** The 5-minute runtime is the cost of working around the silicon errata. Single-session writes can occasionally succeed by luck (this is how the old script worked before block-by-block), but they fail enough to be unusable for active development.
- **Automatic Bluetooth disable.** Could be done with `blueutil`, but installing extra tooling for a one-toggle pre-flight check isn't worth the dependency.
