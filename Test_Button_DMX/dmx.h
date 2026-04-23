#pragma once
#include <SparkFunDMX.h>

extern SparkFunDMX dmxDevice;
extern uint16_t    dmxSerialBufferSize;

void dmxSetup();
void dmxKeepalive();
void dmxSendColor(uint8_t r, uint8_t g, uint8_t b, uint8_t white);
