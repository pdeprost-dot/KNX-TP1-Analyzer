#pragma once

#include <Arduino.h>
#include "acquisition.h"

namespace events {

constexpr uint8_t kCapacity = 24;
enum class CaptureState : uint8_t { Available, NotRetained };

struct Event {
  uint32_t eventId;
  char sessionId[24];
  uint32_t uptimeMs;
  scope::TriggerMode triggerType;
  uint32_t sampleRate;
  uint32_t sampleCount;
  uint32_t triggerIndex;
  uint32_t preTriggerSamples;
  uint32_t postTriggerSamples;
  uint16_t adcMin;
  uint16_t adcMax;
  uint16_t adcMeanBefore;
  uint16_t adcMeanAfter;
  uint32_t captureNumber;
  CaptureState captureState;
  bool rawPersisted;
};

uint32_t record(const Event &event);
void markPersisted(uint32_t eventId);
uint32_t count();
uint8_t retainedCount();
bool newest(uint8_t index, Event &out);
bool find(uint32_t eventId, Event &out);
bool rawAvailable(const Event &event);
const char *triggerName(scope::TriggerMode trigger);
const char *captureStateName(CaptureState state);

} // namespace events
