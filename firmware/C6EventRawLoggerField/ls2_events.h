#pragma once

constexpr uint32_t EVENT_QUEUE_CAPACITY = 32;
constexpr size_t EVENT_BUFFER_BYTES = 8192;

StaticQueue_t eventQueueStorage;
uint8_t eventQueueBytes[EVENT_QUEUE_CAPACITY * sizeof(Event)];
QueueHandle_t eventQueue = nullptr;
File eventsFile;
char eventBuffer[EVENT_BUFFER_BYTES];
size_t eventBufferUsed = 0;
std::atomic<bool> eventWriterActive{false};
uint32_t eventQueueMax = 0;
uint64_t eventMetadataBytes = 0;
uint64_t eventDurationSum = 0;
uint64_t longestEvent = 0;
uint64_t minEventGap = UINT64_MAX;
uint64_t previousEventEnd = 0;
uint32_t completedEventCount = 0;
uint32_t realEvents = 0, syntheticEvents = 0, bothEvents = 0;

bool initEventStreamQueue() {
  eventQueue = xQueueCreateStatic(EVENT_QUEUE_CAPACITY, sizeof(Event), eventQueueBytes,
                                  &eventQueueStorage);
  return eventQueue != nullptr;
}

bool resetEventStream(const String &directory) {
  if (!eventQueue && !initEventStreamQueue()) return false;
  xQueueReset(eventQueue);
  eventsFile = SD.open(directory + "/events.jsonl", FILE_WRITE);
  eventBufferUsed = 0;
  eventWriterActive = false;
  eventQueueMax = 0;
  eventMetadataBytes = 0;
  eventDurationSum = 0;
  longestEvent = 0;
  minEventGap = UINT64_MAX;
  previousEventEnd = 0;
  completedEventCount = 0;
  realEvents = syntheticEvents = bothEvents = 0;
  return bool(eventsFile);
}

bool flushEventBuffer() {
  if (!eventBufferUsed) return true;
  const size_t written = eventsFile ? eventsFile.write(
      reinterpret_cast<const uint8_t *>(eventBuffer), eventBufferUsed) : 0;
  if (written != eventBufferUsed) {
    ++eventMetadataExhaustion;
    ++sdErrors;
    sdIncident = true;
    runState = RS_STOP_REQUESTED;
    active = false;
    return false;
  }
  eventBufferUsed = 0;
  return true;
}

bool appendEventLine(const Event &e) {
  char line[420];
  const int length = snprintf(
      line, sizeof(line),
      "{\"record_type\":\"EVENT\",\"event_id\":\"%llu\","
      "\"sample_start\":\"%llu\",\"sample_trigger\":\"%llu\","
      "\"sample_last_excursion\":\"%llu\",\"sample_end\":\"%llu\","
      "\"trigger_timestamp_us\":\"%llu\",\"end_timestamp_us\":\"%llu\","
      "\"trigger_source\":%u,\"d44_trigger\":%u,\"d44_max\":%u,"
      "\"excursions\":%u,\"adc_min\":%u,\"adc_max\":%u}\n",
      e.id, e.start, e.trigger, e.lastExc, e.end, e.triggerUs, e.endUs,
      e.sourceMask, e.triggerD44, e.maxD44, e.excursions, e.adcMin, e.adcMax);
  if (length <= 0 || size_t(length) >= sizeof(line)) {
    ++eventMetadataExhaustion;
    return false;
  }
  if (eventBufferUsed + size_t(length) > sizeof(eventBuffer) && !flushEventBuffer()) return false;
  memcpy(eventBuffer + eventBufferUsed, line, size_t(length));
  eventBufferUsed += size_t(length);
  eventMetadataBytes += uint64_t(length);
  return true;
}

void drainOneEvent() {
  Event event;
  if (!eventQueue || xQueueReceive(eventQueue, &event, 0) != pdTRUE) return;
  eventWriterActive = true;
  appendEventLine(event);
  eventWriterActive = false;
}

bool queueCompletedEvent(Event &event) {
  const uint64_t duration = event.end >= event.start ? event.end - event.start : 0;
  eventDurationSum += duration;
  if (duration > longestEvent) longestEvent = duration;
  if (completedEventCount && event.start >= previousEventEnd) {
    const uint64_t gap = event.start - previousEventEnd;
    if (gap < minEventGap) minEventGap = gap;
  }
  previousEventEnd = event.end;
  if (event.sourceMask == 1) ++realEvents;
  else if (event.sourceMask == 2) ++syntheticEvents;
  else if (event.sourceMask == 3) ++bothEvents;
  ++completedEventCount;
  if (!eventQueue || xQueueSend(eventQueue, &event, 0) != pdTRUE) {
    ++eventMetadataExhaustion;
    runState = RS_STOP_REQUESTED;
    active = false;
    return false;
  }
  const uint32_t depth = uxQueueMessagesWaiting(eventQueue);
  if (depth > eventQueueMax) eventQueueMax = depth;
  return true;
}

void closeEvent(Event &event, uint64_t sampleEnd, uint64_t timestampUs, bool clampEnd) {
  if (clampEnd && event.end > sampleEnd) event.end = sampleEnd;
  event.endUs = timestampUs;
  queueCompletedEvent(event);
}

bool finishEventStream() {
  while ((eventQueue && uxQueueMessagesWaiting(eventQueue)) || eventWriterActive) delay(1);
  if (!flushEventBuffer()) return false;
  if (eventsFile) {
    eventsFile.flush();
    eventsFile.close();
  }
  return eventMetadataExhaustion == 0;
}
