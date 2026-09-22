#pragma once

#include <Arduino.h>

namespace tp1 {
struct Status {
  uint32_t pulses;
  uint32_t characters;
  uint32_t parityErrors;
  uint32_t timingErrors;
  uint32_t frames;
  uint32_t validKnown;
  uint32_t acks;
  uint32_t validUnknown;
  uint32_t invalidChecksum;
  uint32_t incomplete;
  uint32_t undecoded;
  uint32_t queueDrops;
  uint32_t journalDrops;
  uint8_t lastLength;
  char lastHex[49];
  char lastRoute[34];
  char lastAck[8];
};

bool begin();
void reset();
void feed(uint16_t raw, uint32_t sampleIndex);
void poll();
bool flush();
Status status();
}
