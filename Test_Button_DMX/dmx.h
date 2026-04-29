#pragma once
#include <SparkFunDMX.h>

extern SparkFunDMX dmxDevice;
extern uint16_t    dmxSerialBufferSize;

void dmxSetup();
void dmxKeepalive();
