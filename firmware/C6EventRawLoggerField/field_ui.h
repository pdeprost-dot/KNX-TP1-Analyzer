#pragma once

#include <Arduino.h>

enum class FieldState : uint8_t { Idle, Capturing, Finalizing, Closed };

struct FieldSnapshot {
  FieldState state;
  bool sdReady;
  bool adcReady;
  const char *session;
  uint32_t sessions;
  uint64_t durationUs;
  uint64_t samples;
  uint32_t events;
  uint64_t rawBytes;
  uint32_t d44Max;
  uint32_t recoveredR1;
  uint64_t loss;
  uint32_t dmaOverflow;
  uint32_t poolExhaustion;
  uint32_t sdErrors;
  bool closed;
};

FieldSnapshot fieldSnapshot();
bool fieldStartCapture();
bool fieldStopCapture();
bool fieldNewSession();

namespace fieldui {
bool begin();
void tick();
bool otaBusy();
}
