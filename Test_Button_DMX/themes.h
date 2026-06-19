#pragma once
#include <Arduino.h>
#include "towers.h"

// Theme catalogue (string -> visual behaviour):
//   green        green-fire gradient with 800/3200 flash cycle
//   blue         blue-fire gradient with flash cycle
//   fire         natural-fire gradient with flash cycle
//   simon        global rotating R/B/Y/G across the 4 towers (1 s/beat)
//   rainbow      continuous hue rotation, 90 deg offset per tower
//   warm_white   steady dim warm white (chill ambient)
//   bright_white steady full-bright white
//   candle       warm white with per-tower flicker
//
// `speedPct` is the per-tower speed override (10..400, 100 = normal). It scales
// effective time for all time-based behaviours: flash cycle, Simon beat,
// rainbow hue rotation, candle flicker.
//
// `nowMs` is current millis() at the call site. For flash-pattern themes the
// returned TowerState is zeroed during the OFF phase.
TowerState themeRender(const String& name, uint8_t index, uint32_t nowMs,
                       uint8_t brightness, uint16_t speedPct);
