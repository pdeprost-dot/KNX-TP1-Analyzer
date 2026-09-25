#pragma once

void runLs2EventStress(uint32_t target) {
  if (runState != RS_IDLE || active || target == 0) return;
  char directory[32];
  snprintf(directory, sizeof(directory), "/LS2-STRESS-%08lX", (unsigned long)esp_random());
  if (!SD.mkdir(directory) || !resetEventStream(directory)) {
    Serial.println("{\"type\":\"LS2_STRESS\",\"pass\":false,\"error\":\"open\"}");
    return;
  }
  eventMetadataExhaustion = 0;
  const uint32_t heapStart = ESP.getFreeHeap();
  uint32_t heapMin = heapStart, heap1000 = 0, heap10000 = 0;
  for (uint32_t i = 1; i <= target; ++i) {
    Event event{};
    event.id = i;
    event.start = uint64_t(i - 1) * 1000ULL;
    event.trigger = event.start + 100;
    event.lastExc = event.start + 300;
    event.end = event.start + 500;
    event.triggerUs = uint64_t(i) * 12000ULL;
    event.endUs = event.triggerUs + 6000ULL;
    event.sourceMask = 2;
    event.triggerD44 = event.maxD44 = 501;
    event.excursions = 1;
    event.adcMin = 1000;
    event.adcMax = 2000;
    while (uxQueueSpacesAvailable(eventQueue) == 0) delay(1);
    if (xQueueSend(eventQueue, &event, pdMS_TO_TICKS(100)) != pdTRUE) {
      ++eventMetadataExhaustion;
      break;
    }
    const uint32_t depth = uxQueueMessagesWaiting(eventQueue);
    if (depth > eventQueueMax) eventQueueMax = depth;
    const uint32_t heapNow = ESP.getFreeHeap();
    if (heapNow < heapMin) heapMin = heapNow;
    if (i == 1000) heap1000 = heapNow;
    if (i == 10000) heap10000 = heapNow;
  }
  while (uxQueueMessagesWaiting(eventQueue) || eventWriterActive) delay(1);
  flushEventBuffer();
  if (eventsFile) {
    eventsFile.flush();
    eventsFile.close();
  }
  const uint32_t heapEnd = ESP.getFreeHeap();
  File check = SD.open(String(directory) + "/events.jsonl", FILE_READ);
  uint32_t lines = 0;
  int last = -1;
  uint64_t bytes = 0;
  if (check) {
    bytes = check.size();
    uint8_t block[1024];
    while (check.available()) {
      const size_t count = check.read(block, sizeof(block));
      for (size_t i = 0; i < count; ++i) {
        if (block[i] == '\n') ++lines;
        last = block[i];
      }
    }
    check.close();
  }
  const bool pass = lines == target && last == '\n' && eventMetadataExhaustion == 0;
  Serial.printf(
      "{\"type\":\"LS2_STRESS\",\"pass\":%s,\"events\":%u,\"lines\":%u,"
      "\"heap_start\":%u,\"heap_min\":%u,\"heap_1000\":%u,\"heap_10000\":%u,"
      "\"heap_end\":%u,\"events_bytes\":%llu,\"metadata_queue_max\":%u,"
      "\"metadata_exhaustion\":%u,\"jsonl_final_newline\":%s,\"directory\":\"%s\"}\n",
      pass ? "true" : "false", target, lines, heapStart, heapMin, heap1000, heap10000,
      heapEnd, bytes, eventQueueMax, eventMetadataExhaustion,
      last == '\n' ? "true" : "false", directory);
}
