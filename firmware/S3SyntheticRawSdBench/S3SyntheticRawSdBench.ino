#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_crc.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <errno.h>

// Experimental XIAO ESP32-S3 Sense RAW/SD bench. No ADC, KNX, display,
// touch, Wi-Fi, camera, or microphone is initialized by this sketch.
constexpr uint32_t SAMPLE_RATE = 83333;
constexpr uint32_t CHUNK_SAMPLES = 4096;
constexpr uint32_t CHUNK_BYTES = CHUNK_SAMPLES * sizeof(uint16_t);
constexpr uint32_t CHUNK_COUNT = 12;
constexpr uint32_t SD_HZ = 4000000;
constexpr uint32_t SD_CS = 21, SD_SCK = 7, SD_MISO = 8, SD_MOSI = 9;
constexpr uint64_t SEGMENT_LIMIT = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t CHECKPOINT_CHUNKS = 64;
constexpr uint32_t INDEX_STRIDE_CHUNKS = 256;
#ifndef S3_WRITER_PRIORITY
#define S3_WRITER_PRIORITY 2
#endif
#ifndef S3_WRITER_CORE
#define S3_WRITER_CORE 0
#endif

enum class State : uint8_t { IDLE, RUNNING, STOPPING, CLOSED, FAILED };
struct Chunk {
  uint64_t seq = 0, sampleStart = 0, segmentOffset = 0;
  uint32_t crc = 0;
  uint16_t data[CHUNK_SAMPLES];
};

SPIClass sdSpi(FSPI);
Chunk *chunks[CHUNK_COUNT]{};
QueueHandle_t freeQ = nullptr, readyQ = nullptr;
TaskHandle_t producerHandle = nullptr, writerHandle = nullptr;
File rawFile, mapFile, segmentsFile, indexFile, checkpointsFile, incidentsFile, eventsFile, gapsFile;
String sessionDir;
std::atomic<State> state{State::IDLE};
std::atomic<bool> stopRequested{false}, producerDrained{true}, writerActive{false};
std::atomic<uint64_t> producedSamples{0}, producedChunks{0}, rawBytes{0}, lostSamples{0};
std::atomic<uint32_t> poolExhaustion{0}, sdErrors{0}, r1Count{0}, rawHandleFailures{0}, retryFailures{0}, reopenFailures{0};
std::atomic<uint32_t> faultInjected{0}, syntheticR3Count{0}, gapCount{0};
const char *lastSdErrorSource = "none";
const char *completionStatus = "FAILED";
uint64_t faultR3OnceChunk = 0;
bool faultR3OnceConsumed = false;
uint64_t gapSampleStart = 0, gapSampleEnd = 0;
uint32_t gapSegmentBefore = 0, gapSegmentAfter = 0;
uint64_t gapPreviousStoredEnd = 0, gapResumeSampleStart = 0;
uint64_t firstRawFailureUs = 0;
uint64_t startedUs = 0, closedUs = 0, segmentBytes = 0, segmentSampleStart = 0;
uint32_t segmentIndex = 0, segmentChunks = 0, segmentCrc = 0, sessionCrc = 0;
uint64_t storedChunks = 0, checkpointCount = 0, lastStoredSampleEnd = 0;
char mapBuffer[8192];
size_t mapUsed = 0;
bool sdReady = false, finalPass = false, invariantOk = false;
uint32_t freeMin = CHUNK_COUNT, pendingMax = 0, readyMax = 0;
uint64_t writeLatencyTotalUs = 0, checkpointLatencyTotalUs = 0;
uint32_t writeLatencyMinUs = UINT32_MAX, writeLatencyMaxUs = 0, writeLatencyMaxChunk = 0, writeCount = 0;
uint32_t checkpointLatencyMinUs = UINT32_MAX, checkpointLatencyMaxUs = 0, checkpointLatencyMaxChunk = 0;
uint32_t writerHoldMaxUs = 0, writerHoldMaxChunk = 0;
uint64_t nextChunkDueUs = 0;
uint32_t cadenceRemainder = 0;

void updateQueueStats() {
  const uint32_t freeDepth = freeQ ? uxQueueMessagesWaiting(freeQ) : 0;
  const uint32_t readyDepth = readyQ ? uxQueueMessagesWaiting(readyQ) : 0;
  const uint32_t pending = readyDepth + (writerActive.load() ? 1U : 0U);
  if (freeDepth < freeMin) freeMin = freeDepth;
  if (readyDepth > readyMax) readyMax = readyDepth;
  if (pending > pendingMax) pendingMax = pending;
}

const char *stateName(State value) {
  switch (value) {
    case State::IDLE: return "IDLE";
    case State::RUNNING: return "RUNNING";
    case State::STOPPING: return "STOPPING";
    case State::CLOSED: return "CLOSED";
    default: return "FAILED";
  }
}

String rawName(uint32_t index) {
  char value[24];
  snprintf(value, sizeof(value), "raw-%04u.bin", index);
  return String(value);
}

bool flushMap() {
  if (!mapUsed) return true;
  const size_t written = mapFile ? mapFile.write(reinterpret_cast<uint8_t *>(mapBuffer), mapUsed) : 0;
  if (written != mapUsed) return false;
  mapUsed = 0;
  return true;
}

bool appendMap(const Chunk &chunk) {
  char line[320];
  const int length = snprintf(line, sizeof(line),
      "{\"record_type\":\"CHUNK\",\"seq\":\"%llu\",\"sample_start\":\"%llu\","
      "\"sample_end\":\"%llu\",\"sample_count\":%u,\"segment_index\":%u,"
      "\"segment_offset\":\"%llu\",\"raw_bytes\":%u,\"crc32\":\"%08X\"}\n",
      chunk.seq, chunk.sampleStart, chunk.sampleStart + CHUNK_SAMPLES, CHUNK_SAMPLES,
      segmentIndex, chunk.segmentOffset, CHUNK_BYTES, chunk.crc);
  if (length <= 0 || size_t(length) >= sizeof(line)) return false;
  if (mapUsed + size_t(length) > sizeof(mapBuffer) && !flushMap()) return false;
  memcpy(mapBuffer + mapUsed, line, size_t(length));
  mapUsed += size_t(length);
  return true;
}

bool openSegment(uint64_t sampleStart) {
  const String name = rawName(segmentIndex);
  rawFile = SD.open(sessionDir + "/" + name, FILE_WRITE);
  if (!rawFile) return false;
  segmentBytes = segmentChunks = segmentCrc = 0;
  segmentSampleStart = sampleStart;
  return segmentsFile.printf(
      "{\"record_type\":\"SEGMENT_OPEN\",\"segment_index\":%u,\"filename\":\"%s\","
      "\"sample_start\":\"%llu\",\"state\":\"OPEN\"}\n",
      segmentIndex, name.c_str(), sampleStart) > 0;
}

bool completeSegment() {
  if (!rawFile) return true;
  rawFile.flush();
  rawFile.close();
  const String name = rawName(segmentIndex);
  const bool ok = segmentsFile.printf(
      "{\"record_type\":\"SEGMENT_COMPLETE\",\"segment_index\":%u,\"filename\":\"%s\","
      "\"sample_start\":\"%llu\",\"sample_end\":\"%llu\",\"raw_bytes\":\"%llu\","
      "\"chunk_count\":%u,\"crc32\":\"%08X\",\"state\":\"COMPLETE\"}\n",
      segmentIndex, name.c_str(), segmentSampleStart, lastStoredSampleEnd,
      segmentBytes, segmentChunks, segmentCrc) > 0;
  segmentsFile.flush();
  return ok;
}

bool abandonSegment(const char *reason) {
  if (!rawFile) return true;
  rawFile.flush();
  rawFile.close();
  const String name = rawName(segmentIndex);
  const bool ok = segmentsFile.printf(
      "{\"record_type\":\"SEGMENT_ABANDONED\",\"segment_index\":%u,\"filename\":\"%s\","
      "\"sample_start\":\"%llu\",\"confirmed_sample_end\":\"%llu\",\"confirmed_raw_bytes\":\"%llu\","
      "\"confirmed_chunk_count\":%u,\"confirmed_crc32\":\"%08X\",\"state\":\"ABANDONED\",\"reason\":\"%s\"}\n",
      segmentIndex, name.c_str(), segmentSampleStart, lastStoredSampleEnd,
      segmentBytes, segmentChunks, segmentCrc, reason) > 0;
  segmentsFile.flush();
  return ok;
}

bool prepareSegment(uint64_t sampleStart) {
  if (!rawFile && !openSegment(sampleStart)) return false;
  if (segmentChunks && segmentBytes + CHUNK_BYTES > SEGMENT_LIMIT) {
    if (!completeSegment()) return false;
    ++segmentIndex;
    if (!openSegment(sampleStart)) return false;
  }
  return true;
}

bool writeCheckpoint() {
  if (!storedChunks || (storedChunks % CHECKPOINT_CHUNKS) != 0) return true;
  const uint64_t started = esp_timer_get_time();
  if (!flushMap()) return false;
  rawFile.flush(); mapFile.flush(); indexFile.flush(); eventsFile.flush();
  const uint32_t recordCrc = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&sessionCrc), sizeof(sessionCrc));
  const bool ok = checkpointsFile.printf(
      "{\"record_type\":\"CHECKPOINT\",\"checkpoint_id\":\"%llu\","
      "\"last_chunk_ordinal\":\"%llu\",\"sample_end\":\"%llu\","
      "\"session_raw_bytes\":\"%llu\",\"session_crc32\":\"%08X\","
      "\"record_crc32\":\"%08X\"}\n",
      checkpointCount + 1, storedChunks, lastStoredSampleEnd, rawBytes.load(), sessionCrc, recordCrc) > 0;
  if (ok) { checkpointsFile.flush(); ++checkpointCount; }
  const uint32_t elapsed = uint32_t(esp_timer_get_time() - started);
  checkpointLatencyTotalUs += elapsed;
  if (elapsed < checkpointLatencyMinUs) checkpointLatencyMinUs = elapsed;
  if (elapsed > checkpointLatencyMaxUs) {
    checkpointLatencyMaxUs = elapsed;
    checkpointLatencyMaxChunk = uint32_t(storedChunks);
  }
  return ok;
}

void failSd(const char *source) {
  lastSdErrorSource = source;
  ++sdErrors;
  Serial.printf("{\"type\":\"SD_ERROR\",\"source\":\"%s\",\"count\":%u}\n", source, sdErrors.load());
  stopRequested = true;
  state = State::FAILED;
}

bool injectR3Gap(Chunk &chunk) {
  const uint32_t before = segmentIndex;
  const uint32_t after = before + 1;
  if (!abandonSegment("INJECTED_RAW_RETRY_FAILED")) {
    failSd("segment_abandon");
    return false;
  }
  const uint64_t sampleEnd = chunk.sampleStart + CHUNK_SAMPLES;
  if (!gapsFile.printf(
      "{\"record_type\":\"GAP\",\"gap_id\":\"1\",\"kind\":\"KNOWN_CHUNK_LOSS\","
      "\"cause\":\"INJECTED_RAW_RETRY_FAILED\",\"sample_start\":\"%llu\",\"sample_end\":\"%llu\","
      "\"lost_raw_samples\":\"%u\",\"chunk_seq\":\"%llu\",\"segment_before\":%u,"
      "\"segment_after\":%u,\"recovered\":true}\n",
      chunk.sampleStart, sampleEnd, CHUNK_SAMPLES, chunk.seq, before, after)) {
    failSd("gap");
    return false;
  }
  gapsFile.flush();
  incidentsFile.printf(
      "{\"incident\":\"SYNTHETIC-R3-1\",\"time_us\":\"%llu\",\"chunk_start\":\"%llu\","
      "\"chunk_seq\":\"%llu\",\"requested\":%u,\"returned\":0,\"retry_returned\":0,"
      "\"result\":\"SYNTHETIC_R3\",\"physical_io_attempted\":false}\n",
      esp_timer_get_time() - startedUs, chunk.sampleStart, chunk.seq, CHUNK_BYTES);
  incidentsFile.flush();
  faultR3OnceConsumed = true;
  ++faultInjected;
  ++syntheticR3Count;
  ++gapCount;
  lostSamples += CHUNK_SAMPLES;
  gapSampleStart = chunk.sampleStart;
  gapSampleEnd = sampleEnd;
  gapSegmentBefore = before;
  gapSegmentAfter = after;
  gapPreviousStoredEnd = lastStoredSampleEnd;
  segmentIndex = after;
  segmentBytes = segmentChunks = segmentCrc = 0;
  Serial.printf("{\"type\":\"FAULT_INJECTED\",\"fault\":\"R3_ONCE\",\"chunk_seq\":\"%llu\","
                "\"sample_start\":\"%llu\",\"sample_end\":\"%llu\",\"segment_before\":%u,"
                "\"segment_after\":%u,\"physical_io_attempted\":false}\n",
                chunk.seq, chunk.sampleStart, sampleEnd, before, after);
  return true;
}

bool writeChunk(Chunk &chunk) {
  if (!prepareSegment(chunk.sampleStart)) { failSd(rawFile ? "other_segment_metadata" : "raw_initial"); return false; }
  if (faultR3OnceChunk && !faultR3OnceConsumed && chunk.seq == faultR3OnceChunk)
    return injectR3Gap(chunk);
  chunk.segmentOffset = segmentBytes;
  errno = 0;
  const uint64_t t0 = esp_timer_get_time();
  size_t written = rawFile.write(reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  const uint32_t latencyUs = uint32_t(esp_timer_get_time() - t0);
  writeLatencyTotalUs += latencyUs;
  ++writeCount;
  if (latencyUs < writeLatencyMinUs) writeLatencyMinUs = latencyUs;
  if (latencyUs > writeLatencyMaxUs) {
    writeLatencyMaxUs = latencyUs;
    writeLatencyMaxChunk = uint32_t(chunk.seq);
  }
  const int firstErrno = errno;
  if (written != CHUNK_BYTES) {
    ++rawHandleFailures;
    if (!firstRawFailureUs) firstRawFailureUs = esp_timer_get_time() - startedUs;
    Serial.printf("{\"type\":\"RAW_FAILURE\",\"incident\":%u,\"time_us\":\"%llu\",\"errno\":%d,\"returned\":%u}\n",
                  rawHandleFailures.load(), esp_timer_get_time() - startedUs, firstErrno, unsigned(written));
    rawFile.close();
    errno = 0;
    rawFile = SD.open(sessionDir + "/" + rawName(segmentIndex), FILE_APPEND);
    const int reopenErrno = errno;
    const uint64_t reopenSize = rawFile ? rawFile.size() : 0;
    if (!rawFile) ++reopenFailures;
    const bool coherent = rawFile && reopenSize == chunk.segmentOffset && rawFile.position() == chunk.segmentOffset;
    size_t retryWritten = 0;
    int retryErrno = 0;
    if (coherent) {
      errno = 0;
      retryWritten = rawFile.write(reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
      retryErrno = errno;
    }
    const char *result = !rawFile ? "R4" : !coherent ? "R2" : retryWritten == CHUNK_BYTES ? "R1" : "R3";
    incidentsFile.printf(
        "{\"incident\":%u,\"time_us\":\"%llu\",\"sample_index\":\"%llu\","
        "\"chunk_start\":\"%llu\",\"errno\":%d,\"requested\":%u,\"returned\":%u,"
        "\"latency_us\":%u,\"reopen_errno\":%d,\"reopen_size\":\"%llu\","
        "\"retry_returned\":%u,\"retry_errno\":%d,\"result\":\"%s\"}\n",
        rawHandleFailures.load(), esp_timer_get_time() - startedUs, producedSamples.load(),
        chunk.sampleStart, firstErrno, CHUNK_BYTES, unsigned(written), latencyUs,
        reopenErrno, reopenSize, unsigned(retryWritten), retryErrno, result);
    incidentsFile.flush();
    if (retryWritten == CHUNK_BYTES) { ++r1Count; written = retryWritten; }
    else { ++retryFailures; lostSamples += CHUNK_SAMPLES - written / 2; failSd(!rawFile ? "reopen" : "raw_retry"); return false; }
  }
  chunk.crc = esp_crc32_le(0, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  sessionCrc = esp_crc32_le(sessionCrc, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  segmentCrc = esp_crc32_le(segmentCrc, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  if ((storedChunks % INDEX_STRIDE_CHUNKS) == 0) {
    indexFile.printf("{\"record_type\":\"CHUNK_INDEX\",\"chunk_ordinal\":\"%llu\","
                     "\"sample_start\":\"%llu\",\"segment_index\":%u,\"segment_offset\":\"%llu\"}\n",
                     storedChunks, chunk.sampleStart, segmentIndex, chunk.segmentOffset);
  }
  if (!appendMap(chunk)) { failSd("map"); return false; }
  rawBytes += CHUNK_BYTES;
  segmentBytes += CHUNK_BYTES;
  ++segmentChunks; ++storedChunks;
  lastStoredSampleEnd = chunk.sampleStart + CHUNK_SAMPLES;
  if (gapCount.load() && !gapResumeSampleStart && chunk.sampleStart >= gapSampleEnd)
    gapResumeSampleStart = chunk.sampleStart;
  if (!writeCheckpoint()) { failSd("checkpoint"); return false; }
  return true;
}

uint16_t syntheticSample(uint64_t index) {
  return uint16_t((index ^ (index >> 16) ^ 0x5A5AULL) & 0xFFFFU);
}

void producerTask(void *) {
  for (;;) {
    if (state.load() != State::RUNNING || stopRequested.load()) {
      producerDrained = true;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    producerDrained = false;
    while (!stopRequested.load()) {
      const int64_t remaining = int64_t(nextChunkDueUs - esp_timer_get_time());
      if (remaining <= 0) break;
      if (remaining > 2000) vTaskDelay(pdMS_TO_TICKS(1)); else taskYIELD();
    }
    if (stopRequested.load()) continue;
    Chunk *chunk = nullptr;
    if (xQueueReceive(freeQ, &chunk, 0) != pdTRUE) {
      ++poolExhaustion;
      lostSamples += CHUNK_SAMPLES;
      state = State::FAILED;
      stopRequested = true;
      continue;
    }
    updateQueueStats();
    const uint64_t start = producedSamples.load();
    chunk->seq = producedChunks.load() + 1;
    chunk->sampleStart = start;
    for (uint32_t i = 0; i < CHUNK_SAMPLES; ++i) chunk->data[i] = syntheticSample(start + i);
    producedSamples += CHUNK_SAMPLES;
    ++producedChunks;
    if (xQueueSend(readyQ, &chunk, 0) != pdTRUE) {
      ++poolExhaustion; lostSamples += CHUNK_SAMPLES; state = State::FAILED; stopRequested = true;
      xQueueSend(freeQ, &chunk, portMAX_DELAY);
      continue;
    }
    updateQueueStats();
    uint32_t intervalUs = 49152;
    cadenceRemainder += 16384;
    if (cadenceRemainder >= SAMPLE_RATE) { cadenceRemainder -= SAMPLE_RATE; ++intervalUs; }
    const uint64_t now = esp_timer_get_time();
    nextChunkDueUs = (nextChunkDueUs > now ? nextChunkDueUs : now) + intervalUs;
  }
}

void writerTask(void *) {
  for (;;) {
    Chunk *chunk = nullptr;
    if (xQueueReceive(readyQ, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
      const uint64_t holdStarted = esp_timer_get_time();
      writerActive = true;
      updateQueueStats();
      writeChunk(*chunk);
      writerActive = false;
      xQueueSend(freeQ, &chunk, portMAX_DELAY);
      const uint32_t heldUs = uint32_t(esp_timer_get_time() - holdStarted);
      if (heldUs > writerHoldMaxUs) { writerHoldMaxUs = heldUs; writerHoldMaxChunk = uint32_t(chunk->seq); }
      updateQueueStats();
    }
  }
}

void resetCounters() {
  producedSamples = producedChunks = rawBytes = lostSamples = 0;
  poolExhaustion = sdErrors = r1Count = rawHandleFailures = retryFailures = reopenFailures = 0;
  faultInjected = syntheticR3Count = gapCount = 0;
  lastSdErrorSource = "none";
  completionStatus = "FAILED";
  faultR3OnceConsumed = false;
  gapSampleStart = gapSampleEnd = 0;
  gapSegmentBefore = gapSegmentAfter = 0;
  gapPreviousStoredEnd = gapResumeSampleStart = 0;
  firstRawFailureUs = 0;
  segmentIndex = segmentChunks = segmentCrc = sessionCrc = 0;
  segmentBytes = storedChunks = checkpointCount = lastStoredSampleEnd = 0;
  mapUsed = 0; finalPass = invariantOk = false;
  freeMin = CHUNK_COUNT; pendingMax = readyMax = 0;
  writeLatencyTotalUs = checkpointLatencyTotalUs = 0;
  writeLatencyMinUs = checkpointLatencyMinUs = UINT32_MAX;
  writeLatencyMaxUs = checkpointLatencyMaxUs = writerHoldMaxUs = 0;
  writeLatencyMaxChunk = checkpointLatencyMaxChunk = writerHoldMaxChunk = writeCount = 0;
  cadenceRemainder = 0;
}

bool startRun() {
  if (!sdReady || (state.load() != State::IDLE && state.load() != State::CLOSED)) return false;
  resetCounters();
  char name[32]; snprintf(name, sizeof(name), "/S3RAW-%08lX", (unsigned long)esp_random());
  sessionDir = name;
  if (!SD.mkdir(sessionDir)) return false;
  mapFile = SD.open(sessionDir + "/chunks.jsonl", FILE_WRITE);
  segmentsFile = SD.open(sessionDir + "/segments.jsonl", FILE_WRITE);
  indexFile = SD.open(sessionDir + "/chunk-index.jsonl", FILE_WRITE);
  checkpointsFile = SD.open(sessionDir + "/checkpoints.jsonl", FILE_WRITE);
  incidentsFile = SD.open(sessionDir + "/sd-incidents.jsonl", FILE_WRITE);
  eventsFile = SD.open(sessionDir + "/events.jsonl", FILE_WRITE);
  gapsFile = SD.open(sessionDir + "/gaps.jsonl", FILE_WRITE);
  if (!mapFile || !segmentsFile || !indexFile || !checkpointsFile || !incidentsFile || !eventsFile || !gapsFile) {
    state = State::FAILED; return false;
  }
  File session = SD.open(sessionDir + "/session-start.json", FILE_WRITE);
  if (!session) { state = State::FAILED; return false; }
  session.printf("{\"schema_version\":\"s3-synthetic-raw-sd-bench-1.0\",\"sample_rate_hz\":%u,"
                 "\"sample_type\":\"uint16_le\",\"chunk_samples\":%u,\"chunk_bytes\":%u,"
                 "\"pattern\":\"index_xor_index_shift16_xor_5A5A\",\"camera\":false,\"microphone\":false}\n",
                 SAMPLE_RATE, CHUNK_SAMPLES, CHUNK_BYTES);
  session.flush(); session.close();
  stopRequested = false; producerDrained = false;
  startedUs = esp_timer_get_time(); closedUs = 0;
  nextChunkDueUs = startedUs + 49152;
  state = State::RUNNING;
  Serial.printf("{\"type\":\"START\",\"dir\":\"%s\",\"sample_rate_hz\":%u}\n", sessionDir.c_str(), SAMPLE_RATE);
  return true;
}

void finalizeRun() {
  State before = state.load();
  if (before != State::RUNNING && before != State::FAILED) return;
  if (before == State::RUNNING) state = State::STOPPING;
  stopRequested = true;
  const uint32_t drainDeadline = millis() + 3000;
  while (!producerDrained.load() && int32_t(drainDeadline - millis()) > 0) delay(1);
  while ((uxQueueMessagesWaiting(readyQ) || writerActive.load()) && int32_t(drainDeadline - millis()) > 0) delay(1);
  flushMap();
  completeSegment();
  mapFile.flush(); mapFile.close(); segmentsFile.close(); indexFile.flush(); indexFile.close();
  checkpointsFile.flush(); checkpointsFile.close(); incidentsFile.flush(); incidentsFile.close();
  eventsFile.flush(); eventsFile.close(); gapsFile.flush(); gapsFile.close();
  closedUs = esp_timer_get_time();
  invariantOk = producedSamples.load() == (rawBytes.load() / 2ULL) + lostSamples.load();
  const bool faultExpected = faultR3OnceChunk != 0;
  const bool exactInjectedGap = faultExpected && faultR3OnceConsumed && faultInjected.load() == 1 &&
      syntheticR3Count.load() == 1 && gapCount.load() == 1 && lostSamples.load() == CHUNK_SAMPLES &&
      gapSampleEnd - gapSampleStart == CHUNK_SAMPLES && gapSegmentAfter == gapSegmentBefore + 1 &&
      gapPreviousStoredEnd == gapSampleStart && gapResumeSampleStart == gapSampleEnd;
  const bool structurallyHealthy = before != State::FAILED && sdErrors.load() == 0 && poolExhaustion.load() == 0 &&
      retryFailures.load() == 0 && invariantOk;
  finalPass = structurallyHealthy && (faultExpected ? exactInjectedGap : lostSamples.load() == 0);
  if (!structurallyHealthy || !finalPass) completionStatus = "FAILED";
  else if (exactInjectedGap) completionStatus = "PARTIAL";
  else if (r1Count.load()) completionStatus = "COMPLETE_WITH_RECOVERED_ERRORS";
  else completionStatus = "COMPLETE";
  File result = SD.open(sessionDir + "/test-result.json", FILE_WRITE);
  if (result) {
    result.printf("{\"pass\":%s,\"schema_version\":\"s3-synthetic-raw-sd-bench-1.0\","
                  "\"duration_us\":\"%llu\",\"samples\":\"%llu\",\"chunks\":\"%llu\","
                  "\"raw_bytes\":\"%llu\",\"raw_handle_failures\":%u,\"recovered_R1\":%u,"
                  "\"retry_failures\":%u,\"reopen_failures\":%u,\"sd_errors\":%u,\"data_loss\":\"%llu\","
                  "\"pool_exhaustion\":%u,\"free_min\":%u,\"pending_max\":%u,\"ready_max\":%u,"
                  "\"write_us_min\":%u,\"write_us_avg\":%.3f,\"write_us_max\":%u,\"write_max_chunk\":%u,"
                  "\"checkpoint_us_min\":%u,\"checkpoint_us_avg\":%.3f,\"checkpoint_us_max\":%u,"
                  "\"checkpoint_max_chunk\":%u,\"writer_hold_us_max\":%u,\"writer_hold_max_chunk\":%u,"
                  "\"session_crc32\":\"%08X\",\"invariant\":%s,\"closed\":true,"
                  "\"lifecycle\":\"CLOSED\",\"completion_status\":\"%s\",\"resilience_pass\":%s,"
                  "\"fault_injected\":%u,\"real_io_errors\":%u,\"synthetic_r3_count\":%u,"
                  "\"gap_count\":%u,\"gap_sample_start\":\"%llu\",\"gap_sample_end\":\"%llu\","
                  "\"gap_previous_stored_end\":\"%llu\",\"gap_resume_sample_start\":\"%llu\"}\n",
                  finalPass ? "true" : "false", closedUs - startedUs, producedSamples.load(), producedChunks.load(),
                  rawBytes.load(), rawHandleFailures.load(), r1Count.load(), retryFailures.load(), reopenFailures.load(),
                  sdErrors.load(), lostSamples.load(), poolExhaustion.load(), freeMin, pendingMax, readyMax,
                  writeCount ? writeLatencyMinUs : 0, writeCount ? double(writeLatencyTotalUs) / writeCount : 0,
                  writeLatencyMaxUs, writeLatencyMaxChunk,
                  checkpointCount ? checkpointLatencyMinUs : 0,
                  checkpointCount ? double(checkpointLatencyTotalUs) / checkpointCount : 0,
                  checkpointLatencyMaxUs, checkpointLatencyMaxChunk, writerHoldMaxUs, writerHoldMaxChunk,
                  sessionCrc, invariantOk ? "true" : "false", completionStatus,
                  finalPass ? "true" : "false", faultInjected.load(), rawHandleFailures.load(),
                  syntheticR3Count.load(), gapCount.load(), gapSampleStart, gapSampleEnd,
                  gapPreviousStoredEnd, gapResumeSampleStart);
    result.flush(); result.close();
  } else {
    lastSdErrorSource = "finalization"; ++sdErrors; finalPass = false;
    Serial.printf("{\"type\":\"SD_ERROR\",\"source\":\"finalization\",\"count\":%u}\n", sdErrors.load());
  }
  state = finalPass ? State::CLOSED : State::FAILED;
  Serial.printf("{\"type\":\"FINALIZATION\",\"state\":\"%s\",\"completion_status\":\"%s\","
                "\"pass\":%s,\"crc32\":\"%08X\",\"invariant\":%s}\n",
                stateName(state.load()), completionStatus, finalPass ? "true" : "false",
                sessionCrc, invariantOk ? "true" : "false");
}

void printStatus() {
  const State currentState = state.load();
  const uint64_t end = (currentState == State::CLOSED || currentState == State::FAILED) && closedUs ? closedUs : esp_timer_get_time();
  const uint64_t duration = startedUs ? end - startedUs : 0;
  Serial.printf("{\"type\":\"STATUS\",\"state\":\"%s\",\"duration_us\":\"%llu\","
                "\"chunks\":\"%llu\",\"samples\":\"%llu\",\"raw_bytes\":\"%llu\","
                "\"R1\":%u,\"raw_handle_failures\":%u,\"retry_failures\":%u,\"reopen_failures\":%u,"
                "\"sd_errors\":%u,\"sd_error_source\":\"%s\",\"loss\":\"%llu\",\"pool_exhaustion\":%u,"
                "\"free_min\":%u,\"pending_max\":%u,\"ready_current\":%u,\"ready_max\":%u,"
                "\"write_us_min\":%u,\"write_us_avg\":%.3f,\"write_us_max\":%u,"
                "\"checkpoint_us_min\":%u,\"checkpoint_us_avg\":%.3f,\"checkpoint_us_max\":%u,"
                "\"writer_hold_us_max\":%u,\"first_raw_failure_us\":\"%llu\",\"heap_internal_free\":%u,\"heap_internal_min\":%u,"
                "\"heap_internal_largest\":%u,\"heap_dma_free\":%u,\"heap_dma_min\":%u,"
                "\"heap_dma_largest\":%u,\"crc32\":\"%08X\",\"invariant\":%s,\"pass\":%s,"
                "\"completion_status\":\"%s\",\"fault_r3_once_chunk\":\"%llu\","
                "\"fault_consumed\":%s,\"fault_injected\":%u,\"real_io_errors\":%u,"
                "\"synthetic_r3_count\":%u,\"gap_count\":%u}\n",
                stateName(currentState), duration, producedChunks.load(), producedSamples.load(), rawBytes.load(),
                r1Count.load(), rawHandleFailures.load(), retryFailures.load(), reopenFailures.load(),
                sdErrors.load(), lastSdErrorSource, lostSamples.load(), poolExhaustion.load(), freeMin, pendingMax,
                readyQ ? uxQueueMessagesWaiting(readyQ) : 0, readyMax,
                writeCount ? writeLatencyMinUs : 0, writeCount ? double(writeLatencyTotalUs) / writeCount : 0,
                writeLatencyMaxUs, checkpointCount ? checkpointLatencyMinUs : 0,
                checkpointCount ? double(checkpointLatencyTotalUs) / checkpointCount : 0,
                checkpointLatencyMaxUs, writerHoldMaxUs, firstRawFailureUs,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_free_size(MALLOC_CAP_DMA), heap_caps_get_minimum_free_size(MALLOC_CAP_DMA),
                heap_caps_get_largest_free_block(MALLOC_CAP_DMA), sessionCrc,
                invariantOk ? "true" : "false", finalPass ? "true" : "false", completionStatus,
                faultR3OnceChunk, faultR3OnceConsumed ? "true" : "false", faultInjected.load(),
                rawHandleFailures.load(), syntheticR3Count.load(), gapCount.load());
}

const char *sdTypeName(sdcard_type_t type) {
  switch (type) {
    case CARD_MMC: return "MMC";
    case CARD_SD: return "SDSC";
    case CARD_SDHC: return "SDHC";
    default: return "UNKNOWN";
  }
}

void printSdInfo() {
  const State currentState = state.load();
  if (currentState == State::RUNNING || currentState == State::STOPPING) {
    Serial.printf("{\"type\":\"SDINFO\",\"ok\":false,\"error\":\"acquisition_active\",\"state\":\"%s\"}\n",
                  stateName(currentState));
    return;
  }
  const sdcard_type_t cardType = SD.cardType();
  Serial.printf("{\"type\":\"SDINFO\",\"ok\":%s,\"card_type_code\":%u,\"card_type\":\"%s\","
                "\"capacity_bytes\":\"%llu\",\"sector_count\":%u,\"sector_size_bytes\":%u,"
                "\"filesystem_total_bytes\":\"%llu\",\"filesystem_used_bytes\":\"%llu\","
                "\"cid\":null,\"csd\":null,\"mid\":null,\"oid\":null,\"pnm\":null,"
                "\"prv\":null,\"psn\":null,\"mdt\":null}\n",
                sdReady && cardType != CARD_NONE ? "true" : "false", unsigned(cardType), sdTypeName(cardType),
                SD.cardSize(), unsigned(SD.numSectors()), unsigned(SD.sectorSize()),
                SD.totalBytes(), SD.usedBytes());
}

void printFile(const String &path, const char *label) {
  File file = SD.open(path, FILE_READ);
  const uint64_t fileBytes = file ? uint64_t(file.size()) : 0;
  Serial.printf("{\"type\":\"INSPECT_FILE\",\"name\":\"%s\",\"present\":%s,\"bytes\":\"%llu\"}\n",
                label, file ? "true" : "false", fileBytes);
  while (file && file.available()) Serial.write(file.read());
  if (file) file.close();
}

void inspectSession(const String &folder, uint64_t targetSeq) {
  if (state.load() == State::RUNNING || state.load() == State::STOPPING) {
    Serial.println("ERROR INSPECT ACQUISITION_ACTIVE");
    return;
  }
  const String base = "/S3RAW-" + folder;
  const uint64_t targetStart = (targetSeq - 1) * uint64_t(CHUNK_SAMPLES);
  uint32_t actualCrc = 0;
  bool patternOk = true;
  uint64_t actualBytes = 0;
  for (uint32_t index = 0; index < 2; ++index) {
    File file = SD.open(base + "/" + rawName(index), FILE_READ);
    const uint64_t size = file ? file.size() : 0;
    Serial.printf("{\"type\":\"INSPECT_RAW\",\"segment_index\":%u,\"present\":%s,\"bytes\":\"%llu\"}\n",
                  index, file ? "true" : "false", size);
    uint64_t sample = index == 0 ? 0 : targetStart + CHUNK_SAMPLES;
    uint8_t buffer[512];
    while (file && file.available()) {
      const size_t count = file.read(buffer, sizeof(buffer));
      actualCrc = esp_crc32_le(actualCrc, buffer, count);
      actualBytes += count;
      for (size_t offset = 0; offset + 1 < count; offset += 2, ++sample) {
        const uint16_t value = uint16_t(buffer[offset]) | (uint16_t(buffer[offset + 1]) << 8);
        if (value != syntheticSample(sample)) patternOk = false;
      }
    }
    if (file) file.close();
  }
  File chunksFile = SD.open(base + "/chunks.jsonl", FILE_READ);
  bool beforeFound = false, targetFound = false, afterFound = false;
  const String beforeNeedle = "\"seq\":\"" + String(targetSeq - 1) + "\"";
  const String targetNeedle = "\"seq\":\"" + String(targetSeq) + "\"";
  const String afterNeedle = "\"seq\":\"" + String(targetSeq + 1) + "\"";
  while (chunksFile && chunksFile.available()) {
    const String line = chunksFile.readStringUntil('\n');
    if (line.indexOf(beforeNeedle) >= 0) { beforeFound = true; Serial.println(line); }
    if (line.indexOf(targetNeedle) >= 0) { targetFound = true; Serial.println(line); }
    if (line.indexOf(afterNeedle) >= 0) { afterFound = true; Serial.println(line); }
  }
  if (chunksFile) chunksFile.close();
  Serial.printf("{\"type\":\"INSPECT_SUMMARY\",\"raw_bytes\":\"%llu\",\"crc32\":\"%08X\","
                "\"pattern_ok\":%s,\"chunk_before_present\":%s,\"chunk_target_present\":%s,"
                "\"chunk_after_present\":%s}\n",
                actualBytes, actualCrc, patternOk ? "true" : "false", beforeFound ? "true" : "false",
                targetFound ? "true" : "false", afterFound ? "true" : "false");
  printFile(base + "/gaps.jsonl", "gaps.jsonl");
  printFile(base + "/segments.jsonl", "segments.jsonl");
  printFile(base + "/test-result.json", "test-result.json");
}

#ifndef S3_NETWORK_ENABLED
#define S3_NETWORK_ENABLED 1
#endif
#if S3_NETWORK_ENABLED
namespace s3net { void printDiagnostics(); }
#endif
void handleCommand(String command) {
  command.trim(); command.toUpperCase();
  if (command == "STATUS") printStatus();
  else if (command == "SDINFO") printSdInfo();
  else if (command.startsWith("INSPECT ")) {
    const int separator = command.indexOf(' ', 8);
    if (separator < 0) Serial.println("ERROR INSPECT ARGUMENTS");
    else {
      const String folder = command.substring(8, separator);
      const uint64_t target = strtoull(command.substring(separator + 1).c_str(), nullptr, 10);
      if (folder.length() != 8 || !target) Serial.println("ERROR INSPECT ARGUMENTS");
      else inspectSession(folder, target);
    }
  }
#if S3_NETWORK_ENABLED
  else if (command == "NETSTATUS") s3net::printDiagnostics();
#endif
  else if (command.startsWith("FAULT R3_ONCE ")) {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else {
      const uint64_t target = strtoull(command.substring(14).c_str(), nullptr, 10);
      if (!target) Serial.println("ERROR FAULT CHUNK_SEQ");
      else { faultR3OnceChunk = target; faultR3OnceConsumed = false; Serial.printf("OK FAULT R3_ONCE %llu\n", target); }
    }
  } else if (command == "FAULT OFF") {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else { faultR3OnceChunk = 0; faultR3OnceConsumed = false; Serial.println("OK FAULT OFF"); }
  } else if (command == "START") Serial.println(startRun() ? "OK START" : "ERROR START");
  else if (command == "STOP") {
    if (state.load() != State::RUNNING) Serial.println("ERROR STOP");
    else { finalizeRun(); Serial.println("OK STOP"); }
  } else if (command.length()) Serial.println("ERROR COMMAND");
}

#if S3_NETWORK_ENABLED
#include "s3_network.h"
#endif

void setup() {
  Serial.begin(115200);
  delay(1200);
  sdSpi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  sdReady = SD.begin(SD_CS, sdSpi, SD_HZ, "/sd", 8);
  freeQ = xQueueCreate(CHUNK_COUNT, sizeof(Chunk *));
  readyQ = xQueueCreate(CHUNK_COUNT, sizeof(Chunk *));
  bool poolReady = freeQ && readyQ;
  for (uint32_t i = 0; i < CHUNK_COUNT && poolReady; ++i) {
    chunks[i] = static_cast<Chunk *>(heap_caps_calloc(1, sizeof(Chunk), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    poolReady = chunks[i] != nullptr;
    if (poolReady) xQueueSend(freeQ, &chunks[i], 0);
  }
  if (poolReady) {
    xTaskCreatePinnedToCore(producerTask, "synthetic", 4096, nullptr, 4, &producerHandle, 1);
    xTaskCreatePinnedToCore(writerTask, "writer", 4096, nullptr, S3_WRITER_PRIORITY, &writerHandle, S3_WRITER_CORE);
  }
  Serial.printf("{\"type\":\"BOOT\",\"board\":\"XIAO_ESP32S3_SENSE\",\"sd\":%s,"
                "\"card_type\":%u,\"pool\":%s,\"camera\":false,\"microphone\":false,"
                "\"wifi\":false,\"sample_rate_hz\":%u}\n",
                sdReady ? "true" : "false", unsigned(SD.cardType()), poolReady ? "true" : "false", SAMPLE_RATE);
  if (!sdReady || !poolReady) state = State::FAILED;
#if S3_NETWORK_ENABLED
  s3net::begin();
#endif
}

void loop() {
#if S3_NETWORK_ENABLED
  s3net::tick();
#endif
  static String command;
  while (Serial.available()) {
    const char value = char(Serial.read());
    if (value == '\n' || value == '\r') {
      if (command.length()) { handleCommand(command); command = ""; }
    } else if (command.length() < 31) command += value;
  }
  if (state.load() == State::FAILED && stopRequested.load() && !closedUs) finalizeRun();
  delay(1);
}
