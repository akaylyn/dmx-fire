#pragma once
#include <SparkFunDMX.h>

extern SparkFunDMX dmxDevice;
extern uint16_t    dmxSerialBufferSize;

// Shadow buffer mirroring everything written via dmxShadowWrite().
// Index 0 = DMX channel 1. Used by /api/state to expose the last-sent frame.
static const uint16_t DMX_SHADOW_SIZE = 64;
extern uint8_t dmxLastFrame[DMX_SHADOW_SIZE];

void dmxSetup();
void dmxKeepalive();

// Write a byte to the DMX universe AND keep the shadow buffer in sync.
// Replaces direct dmxDevice.writeByte() in towers.cpp / confluence.cpp.
void dmxShadowWrite(uint8_t value, uint16_t ch);
