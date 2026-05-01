#pragma once
#include <Arduino.h>

// Set compile-time log level. Only messages at or below this level are emitted.
// 0 = off, 1 = ERROR, 2 = WARN, 3 = INFO, 4 = DEBUG
#ifndef LOG_LEVEL
#define LOG_LEVEL 3  // INFO by default
#endif

#define LOG_E(fmt, ...) do { if (LOG_LEVEL >= 1) Serial.printf("[ERROR] " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_W(fmt, ...) do { if (LOG_LEVEL >= 2) Serial.printf("[WARN]  " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_I(fmt, ...) do { if (LOG_LEVEL >= 3) Serial.printf("[INFO]  " fmt "\n", ##__VA_ARGS__); } while(0)
#define LOG_D(fmt, ...) do { if (LOG_LEVEL >= 4) Serial.printf("[DEBUG] " fmt "\n", ##__VA_ARGS__); } while(0)
