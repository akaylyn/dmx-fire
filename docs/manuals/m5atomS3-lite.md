# M5AtomS3 Lite

**SKU:** M5ATOMS3L  
**Docs:** https://docs.m5stack.com/en/core/AtomS3%20Lite  
**Board manager:** M5Stack ESP32 core → `m5stack:esp32:m5stack_atoms3`

---

## Specifications

| Parameter | Value |
|-----------|-------|
| MCU | ESP32-S3FN8 (dual-core Xtensa LX7, 240MHz) |
| Flash | 8MB SPI |
| RAM | 512KB SRAM |
| WiFi | 802.11 b/g/n (2.4GHz), integrated 3D antenna |
| Bluetooth | BLE 5.0 |
| USB | USB Type-C (USB-Serial/JTAG) |
| Operating voltage | 5V via USB-C |
| Logic level | 3.3V |
| Size | 24.0 × 24.0 × 9.5 mm |
| Weight | 5.3 g |

---

## Onboard hardware

| Component | Detail |
|-----------|--------|
| RGB LED | WS2812C-2020, GPIO 35 |
| Button | 1× programmable (BtnA), GPIO 41 |
| IR transmitter | GPIO 12 |
| Power IC | SY8089 DC-DC |

---

## GPIO (HY2.0-4P PORTA connector)

| Pin | Colour | Signal | GPIO |
|-----|--------|--------|------|
| 1 | Black | GND | — |
| 2 | Red | 5V | — |
| 3 | Yellow | G2 (UART TX / I2C SCL) | GPIO 2 |
| 4 | White | G1 (UART RX / I2C SDA) | GPIO 1 |

Used in this project: G1 = Serial1 RX, G2 = Serial1 TX (Unit DMX)

## Additional exposed GPIOs

G5, G6, G7, G8, G38, G39 — available on the side pads.

GPIO 39 is used in this project as the external button input (INPUT_PULLUP, active LOW).

---

## Development

| Platform | Notes |
|----------|-------|
| Arduino IDE | Board: M5Stack → M5AtomS3 |
| arduino-cli FQBN | `m5stack:esp32:m5stack_atoms3` |
| ESP-IDF | Supported |
| PlatformIO | Supported |
| UiFlow2 | Supported |

---

## Key setup notes

- Call `M5.Ex_I2C.release()` after `M5.begin()` to free PORTA for Serial1 use
- Set `pinMode()` **after** `M5.begin()` to avoid pin reconfiguration
- USB-Serial/JTAG mode allows flashing without pressing any boot button on most uploads
