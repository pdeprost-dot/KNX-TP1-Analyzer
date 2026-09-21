#include "events.h"

namespace events {
namespace {
Event ring[kCapacity] = {};
uint8_t nextSlot = 0;
uint8_t used = 0;
uint32_t lastRawEventId = 0;
uint32_t totalEvents = 0;

void resolveCapture(Event &event) {
  const auto adc = scope::status();
  event.captureState = event.eventId == lastRawEventId && adc.captured &&
    adc.captureNumber == event.captureNumber ? CaptureState::Available : CaptureState::NotRetained;
}
}

void record(const Event &event) {
  Event item = event;
  item.eventId = ++totalEvents;
  item.captureState = CaptureState::Available;
  ring[nextSlot] = item;
  nextSlot = (nextSlot + 1) % kCapacity;
  if (used < kCapacity) ++used;
  lastRawEventId = item.eventId;
  Serial.printf("{\"type\":\"EVENT_CREATED\",\"event_id\":%lu,\"capture_number\":%lu,\"heap_free\":%u}\n",
                item.eventId, item.captureNumber, ESP.getFreeHeap());
}

uint32_t count() { return totalEvents; }
uint8_t retainedCount() { return used; }

bool newest(uint8_t index, Event &out) {
  if (index >= used) return false;
  const uint8_t slot = (nextSlot + kCapacity - 1 - index) % kCapacity;
  out = ring[slot];
  resolveCapture(out);
  return true;
}

bool find(uint32_t eventId, Event &out) {
  for (uint8_t i = 0; i < used; ++i) {
    if (newest(i, out) && out.eventId == eventId) return true;
  }
  return false;
}

bool rawAvailable(const Event &event) {
  Event current = event;
  resolveCapture(current);
  return current.captureState == CaptureState::Available;
}

const char *triggerName(scope::TriggerMode trigger) {
  switch (trigger) {
    case scope::TriggerMode::Rising: return "rising";
    case scope::TriggerMode::Falling: return "falling";
    default: return "manual";
  }
}

const char *captureStateName(CaptureState state) {
  return state == CaptureState::Available ? "AVAILABLE" : "NOT_RETAINED";
}

} // namespace events
