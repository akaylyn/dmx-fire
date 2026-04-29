# M5Stack Unit DMX

**SKU:** U183  
**Docs:** https://docs.m5stack.com/en/unit/Unit-DMX  
**Interface:** PORT.C (HY2.0-4P Grove)

---

## Specifications

| Parameter | Value |
|-----------|-------|
| Protocol | DMX512 / RS-485 (half-duplex) |
| Transceiver IC | CA-IS3092W |
| Isolation voltage | 5kVrms |
| Max data rate | 500 Kbps (DMX fixed at 250 Kbps) |
| Terminal resistor | 120Ω (built-in, bus-end matching) |
| Operating temp | 0–40°C |
| Dimensions | 51.6 × 24.0 × 32.0 mm |
| Weight | 14.5 g |
| Connector (DMX) | XLR-3 female |

---

## Pinout (HY2.0-4P Grove connector)

| Wire | Function | AtomS3 Lite GPIO |
|------|----------|-----------------|
| Black | GND | GND |
| Red | 5V | 5V |
| Yellow | UART_RX | GPIO 1 (G1) |
| White | UART_TX | GPIO 2 (G2) |

Connect to **PORTA** on the AtomS3 Lite.

---

## XLR-3 (DMX connector)

| Pin | Signal |
|-----|--------|
| 1 | GND / Shield |
| 2 | Data − (cold) |
| 3 | Data + (hot) |

---

## Arduino usage

```cpp
// In setup() — after M5.Ex_I2C.release()
Serial1.begin(DMX_BAUD, DMX_FORMAT, /*RX*/ 1, /*TX*/ 2);
Serial1.setTxBufferSize(512);  // full DMX universe
```

The 120Ω termination resistor is built into the module — no external resistor needed when this unit is the last device on the DMX run.

---

## Package contents

- 1× Unit DMX module
- 1× HY2.0-4P Grove cable (20 cm)
