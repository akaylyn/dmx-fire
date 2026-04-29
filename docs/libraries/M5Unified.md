# M5Unified

**Version:** 0.2.4  
**Source:** https://github.com/m5stack/M5Unified  
**Requires:** M5GFX  
**License:** MIT

Unified Arduino/ESP-IDF library for all M5Stack controllers. Provides hardware abstraction for buttons, display, speaker, IMU, RTC, and power management. Used in this project for board initialisation and the AtomS3 Lite's built-in button.

---

## Installation

Arduino Library Manager: search **M5Unified** (installs M5GFX automatically).

---

## Initialisation

```cpp
#include <M5Unified.h>

void setup() {
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);

    // Release PORTA (G1/G2) so Serial1 can use it for Unit DMX
    M5.Ex_I2C.release();
}

void loop() {
    M5.update();  // must be called every loop to update button state
}
```

> **Note:** Call `M5.Ex_I2C.release()` before using Serial1 on G1/G2 (PORTA). M5.begin() may claim these pins for I2C.

> **Note:** Set `pinMode()` after `M5.begin()` — the board init may reconfigure pins.

---

## Buttons

The M5AtomS3 Lite has one physical button (BtnA).

```cpp
M5.update();  // refresh button state each loop

M5.BtnA.isPressed()    // true while held
M5.BtnA.wasPressed()   // true for one frame on press
M5.BtnA.wasReleased()  // true for one frame on release
```

### External button via `m5::Button_Class`

For GPIO buttons not managed by M5Unified, manually feed the raw state:

```cpp
m5::Button_Class myBtn;

bool pressed = (digitalRead(PIN) == LOW);
myBtn.setRawState(millis(), pressed);

// Then use the same API:
myBtn.isPressed()
myBtn.wasPressed()
myBtn.wasReleased()
```

---

## M5AtomS3 Lite pin reference

| GPIO | Function |
|------|----------|
| G1 | PORTA Yellow (UART RX / I2C SDA) |
| G2 | PORTA White (UART TX / I2C SCL) |
| G5 | Available |
| G6 | Available |
| G7 | Available |
| G8 | Available |
| G38 | Available |
| G39 | Available (used as external button input in this project) |
| G35 | Onboard WS2812C RGB LED data |

---

## Serial / Debug

```cpp
Serial.begin(115200);  // USB CDC — configured by M5.begin() via cfg.serial_baudrate
Serial.println("message");
```

---

## Further reading

- Full API reference: https://github.com/m5stack/M5Unified
- Examples: Arduino IDE → File → Examples → M5Unified → Basic
