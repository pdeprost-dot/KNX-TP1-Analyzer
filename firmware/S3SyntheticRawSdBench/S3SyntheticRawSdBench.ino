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
File rawFile, mapFile, segmentsFile, indexFile, checkpointsFile, incidentsFile, eventsFile;
String sessionDir;
std::atomic<State> state{State::IDLE};
std::atomic<bool> stopRequested{false}, producerDrained{true}, writerActive{false};
std::atomic<uint64_t> producedSamples{0}, producedChunks{0}, rawBytes{0}, lostSamples{0};
std::atomic<uint32_t> poolExhaustion{0}, sdErrors{0}, r1Count{0}, rawHandleFailures{0}, retryFailures{0}, reopenFailures{0};
const char *lastSdErrorSource = "none";
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

bool writeChunk(Chunk &chunk) {
  if (!prepareSegment(chunk.sampleStart)) { failSd(rawFile ? "other_segment_metadata" : "raw_initial"); return false; }
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
  lastSdErrorSource = "none";
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
  if (!mapFile || !segmentsFile || !indexFile || !checkpointsFile || !incidentsFile || !eventsFile) {
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
  eventsFile.flush(); eventsFile.close();
  closedUs = esp_timer_get_time();
  invariantOk = producedSamples.load() == (rawBytes.load() / 2ULL) + lostSamples.load();
  finalPass = before != State::FAILED && sdErrors.load() == 0 && poolExhaustion.load() == 0 &&
              lostSamples.load() == 0 && retryFailures.load() == 0 && invariantOk;
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
                  "\"session_crc32\":\"%08X\",\"invariant\":%s,\"closed\":true}\n",
                  finalPass ? "true" : "false", closedUs - startedUs, producedSamples.load(), producedChunks.load(),
                  rawBytes.load(), rawHandleFailures.load(), r1Count.load(), retryFailures.load(), reopenFailures.load(),
                  sdErrors.load(), lostSamples.load(), poolExhaustion.load(), freeMin, pendingMax, readyMax,
                  writeCount ? writeLatencyMinUs : 0, writeCount ? double(writeLatencyTotalUs) / writeCount : 0,
                  writeLatencyMaxUs, writeLatencyMaxChunk,
                  checkpointCount ? checkpointLatencyMinUs : 0,
                  checkpointCount ? double(checkpointLatencyTotalUs) / checkpointCount : 0,
                  checkpointLatencyMaxUs, checkpointLatencyMaxChunk, writerHoldMaxUs, writerHoldMaxChunk,
                  sessionCrc, invariantOk ? "true" : "false");
    result.flush(); result.close();
  } else {
    lastSdErrorSource = "finalization"; ++sdErrors; finalPass = false;
    Serial.printf("{\"type\":\"SD_ERROR\",\"source\":\"finalization\",\"count\":%u}\n", sdErrors.load());
  }
  state = finalPass ? State::CLOSED : State::FAILED;
  Serial.printf("{\"type\":\"FINALIZATION\",\"state\":\"%s\",\"pass\":%s,\"crc32\":\"%08X\",\"invariant\":%s}\n",
                stateName(state.load()), finalPass ? "true" : "false", sessionCrc, invariantOk ? "true" : "false");
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
                "\"heap_dma_largest\":%u,\"crc32\":\"%08X\",\"invariant\":%s,\"pass\":%s}\n",
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
                invariantOk ? "true" : "false", finalPass ? "true" : "false");
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
#if S3_NETWORK_ENABLED
  else if (command == "NETSTATUS") s3net::printDiagnostics();
#endif
  else if (command == "START") Serial.println(startRun() ? "OK START" : "ERROR START");
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
