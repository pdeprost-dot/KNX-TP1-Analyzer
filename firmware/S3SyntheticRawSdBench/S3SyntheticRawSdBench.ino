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
std::atomic<uint32_t> poolExhaustion{0}, sdErrors{0}, r1Count{0}, rawHandleFailures{0}, retryFailures{0};
uint64_t startedUs = 0, closedUs = 0, segmentBytes = 0, segmentSampleStart = 0;
uint32_t segmentIndex = 0, segmentChunks = 0, segmentCrc = 0, sessionCrc = 0;
uint64_t storedChunks = 0, checkpointCount = 0, lastStoredSampleEnd = 0;
char mapBuffer[8192];
size_t mapUsed = 0;
bool sdReady = false, finalPass = false, invariantOk = false;

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
  return ok;
}

void failSd() {
  ++sdErrors;
  stopRequested = true;
  state = State::FAILED;
}

bool writeChunk(Chunk &chunk) {
  if (!prepareSegment(chunk.sampleStart)) { failSd(); return false; }
  chunk.segmentOffset = segmentBytes;
  errno = 0;
  const uint64_t t0 = esp_timer_get_time();
  size_t written = rawFile.write(reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  const uint32_t latencyUs = uint32_t(esp_timer_get_time() - t0);
  const int firstErrno = errno;
  if (written != CHUNK_BYTES) {
    ++rawHandleFailures;
    rawFile.close();
    errno = 0;
    rawFile = SD.open(sessionDir + "/" + rawName(segmentIndex), FILE_APPEND);
    const int reopenErrno = errno;
    const uint64_t reopenSize = rawFile ? rawFile.size() : 0;
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
    else { ++retryFailures; lostSamples += CHUNK_SAMPLES - written / 2; failSd(); return false; }
  }
  chunk.crc = esp_crc32_le(0, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  sessionCrc = esp_crc32_le(sessionCrc, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  segmentCrc = esp_crc32_le(segmentCrc, reinterpret_cast<uint8_t *>(chunk.data), CHUNK_BYTES);
  if ((storedChunks % INDEX_STRIDE_CHUNKS) == 0) {
    indexFile.printf("{\"record_type\":\"CHUNK_INDEX\",\"chunk_ordinal\":\"%llu\","
                     "\"sample_start\":\"%llu\",\"segment_index\":%u,\"segment_offset\":\"%llu\"}\n",
                     storedChunks, chunk.sampleStart, segmentIndex, chunk.segmentOffset);
  }
  if (!appendMap(chunk)) { failSd(); return false; }
  rawBytes += CHUNK_BYTES;
  segmentBytes += CHUNK_BYTES;
  ++segmentChunks; ++storedChunks;
  lastStoredSampleEnd = chunk.sampleStart + CHUNK_SAMPLES;
  if (!writeCheckpoint()) { failSd(); return false; }
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
    Chunk *chunk = nullptr;
    if (xQueueReceive(freeQ, &chunk, 0) != pdTRUE) {
      ++poolExhaustion;
      lostSamples += CHUNK_SAMPLES;
      state = State::FAILED;
      stopRequested = true;
      continue;
    }
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
    const uint64_t due = startedUs + (producedSamples.load() * 1000000ULL) / SAMPLE_RATE;
    while (!stopRequested.load()) {
      const int64_t remaining = int64_t(due - esp_timer_get_time());
      if (remaining <= 0) break;
      if (remaining > 2000) vTaskDelay(pdMS_TO_TICKS(1)); else taskYIELD();
    }
  }
}

void writerTask(void *) {
  for (;;) {
    Chunk *chunk = nullptr;
    if (xQueueReceive(readyQ, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
      writerActive = true;
      writeChunk(*chunk);
      writerActive = false;
      xQueueSend(freeQ, &chunk, portMAX_DELAY);
    }
  }
}

void resetCounters() {
  producedSamples = producedChunks = rawBytes = lostSamples = 0;
  poolExhaustion = sdErrors = r1Count = rawHandleFailures = retryFailures = 0;
  segmentIndex = segmentChunks = segmentCrc = sessionCrc = 0;
  segmentBytes = storedChunks = checkpointCount = lastStoredSampleEnd = 0;
  mapUsed = 0; finalPass = invariantOk = false;
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
                  "\"retry_failures\":%u,\"sd_errors\":%u,\"data_loss\":\"%llu\","
                  "\"pool_exhaustion\":%u,\"session_crc32\":\"%08X\",\"invariant\":%s,\"closed\":true}\n",
                  finalPass ? "true" : "false", closedUs - startedUs, producedSamples.load(), producedChunks.load(),
                  rawBytes.load(), rawHandleFailures.load(), r1Count.load(), retryFailures.load(), sdErrors.load(),
                  lostSamples.load(), poolExhaustion.load(), sessionCrc, invariantOk ? "true" : "false");
    result.flush(); result.close();
  } else { ++sdErrors; finalPass = false; }
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
                "\"R1\":%u,\"sd_errors\":%u,\"loss\":\"%llu\",\"pool_exhaustion\":%u,"
                "\"crc32\":\"%08X\",\"invariant\":%s,\"pass\":%s}\n",
                stateName(currentState), duration, producedChunks.load(), producedSamples.load(), rawBytes.load(),
                r1Count.load(), sdErrors.load(), lostSamples.load(), poolExhaustion.load(), sessionCrc,
                invariantOk ? "true" : "false", finalPass ? "true" : "false");
}

void handleCommand(String command) {
  command.trim(); command.toUpperCase();
  if (command == "STATUS") printStatus();
  else if (command == "START") Serial.println(startRun() ? "OK START" : "ERROR START");
  else if (command == "STOP") {
    if (state.load() != State::RUNNING) Serial.println("ERROR STOP");
    else { finalizeRun(); Serial.println("OK STOP"); }
  } else if (command.length()) Serial.println("ERROR COMMAND");
}

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
    xTaskCreatePinnedToCore(writerTask, "writer", 4096, nullptr, 2, &writerHandle, 0);
  }
  Serial.printf("{\"type\":\"BOOT\",\"board\":\"XIAO_ESP32S3_SENSE\",\"sd\":%s,"
                "\"card_type\":%u,\"pool\":%s,\"camera\":false,\"microphone\":false,"
                "\"wifi\":false,\"sample_rate_hz\":%u}\n",
                sdReady ? "true" : "false", unsigned(SD.cardType()), poolReady ? "true" : "false", SAMPLE_RATE);
  if (!sdReady || !poolReady) state = State::FAILED;
}

void loop() {
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
