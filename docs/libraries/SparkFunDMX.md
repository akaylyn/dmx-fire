# SparkFun DMX Library

**Version:** 2.0.1  
**Source:** https://github.com/sparkfun/SparkFunDMX  
**License:** MIT

Arduino library for DMX512 output and input over a hardware serial port. Written for the SparkFun ESP32 DMX to LED Shield but works with any UART + RS-485 transceiver (including the M5Stack Unit DMX).

---

## Installation

Arduino Library Manager: search **SparkFun DMX Shield Library**

---

## Constants

```cpp
#define DMX_MAX_CHANNELS   513    // 512 channels + channel 0 (never write to 0)
#define DMX_BREAK_DURATION_MICROS  88
#define DMX_BAUD           250000 // fixed DMX baud rate
#define DMX_FORMAT         SERIAL_8N2
#define DMX_WRITE_DIR      0
#define DMX_READ_DIR       1
```

> **Important:** DMX addresses start at **1**, not 0. Writing to address 0 corrupts the DMX frame.

---

## API

### `begin()`
```cpp
void begin(HardwareSerial& port, uint8_t enPin, uint16_t numChannels);
```
Initialises the DMX driver. The serial port must be opened **before** calling `begin()`.

| Parameter | Description |
|-----------|-------------|
| `port` | Hardware serial port (e.g. `Serial1`) |
| `enPin` | Direction-control pin for the RS-485 transceiver. Pass `255` if not used. |
| `numChannels` | Number of DMX channels to use (1–512) |

---

### `setComDir()`
```cpp
void setComDir(bool comDir);
```
Switch between transmit and receive. Can be changed at runtime.

| Value | Constant | Direction |
|-------|----------|-----------|
| `0` | `DMX_WRITE_DIR` | Transmit (output) |
| `1` | `DMX_READ_DIR` | Receive (input) |

---

### `writeByte()`
```cpp
void writeByte(uint8_t data, uint16_t channel);
```
Write a single value to a single DMX channel (1–512).

---

### `writeBytes()`
```cpp
void writeBytes(uint8_t* data, uint16_t numBytes, uint16_t startChannel = 1);
```
Copy a buffer of values into the DMX frame starting at `startChannel`.

---

### `update()`
```cpp
bool update();
```
- **Write mode:** transmits the current DMX frame over the serial port. Call periodically (at least once per second) to prevent fixtures from timing out.
- **Read mode:** checks for new incoming DMX data.
- Returns `true` on success.

---

### `readByte()`
```cpp
uint8_t readByte(uint16_t channel);
```
Read the last received value for a single channel.

---

### `readBytes()`
```cpp
void readBytes(uint8_t* data, uint16_t numBytes, uint16_t startChannel = 1);
```
Copy received DMX values into a buffer.

---

### `dataAvailable()`
```cpp
bool dataAvailable();
```
Returns `true` if new data has been received since the last `update()` (read mode only).

---

## Usage pattern (this project)

```cpp
// setup()
Serial1.begin(DMX_BAUD, DMX_FORMAT, RX_PIN, TX_PIN);
Serial1.setTxBufferSize(512);
dmxDevice.begin(Serial1, EN_PIN, NUM_CHANNELS);
dmxDevice.setComDir(DMX_WRITE_DIR);

// loop() — keepalive (prevents fixture timeout)
// called once per second when the serial buffer is idle
dmxDevice.update();

// loop() — colour output (~50Hz)
dmxDevice.writeByte(r,     1);
dmxDevice.writeByte(g,     2);
dmxDevice.writeByte(b,     3);
dmxDevice.writeByte(white, 4);
dmxDevice.update();
```
