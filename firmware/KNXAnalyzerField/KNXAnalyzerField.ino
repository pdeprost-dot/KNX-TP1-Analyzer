#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_crc.h>
#include <esp_heap_caps.h>
#include <esp_adc/adc_continuous.h>
#include <mbedtls/sha256.h>
#include <soc/soc_caps.h>
#include <atomic>
#include <errno.h>
#include <time.h>
#include "board_xiao_esp32s3.h"
#include "activity_detector.h"
#include "auto_calibration.h"

#define S3_NETWORK_ENABLED 1
constexpr const char *FIRMWARE_VERSION = "KNXAnalyzerField-s3-analog-v0.8.1-range-write-fix";

// Headless KNX Analyzer Field bring-up. Real ADC; no KNX bus, display, touch,
// camera, microphone, or future UART is initialized in this milestone.
constexpr uint32_t SAMPLE_RATE = 83333;
constexpr uint32_t CHUNK_SAMPLES = 4096;
constexpr uint32_t CHUNK_BYTES = CHUNK_SAMPLES * sizeof(uint16_t);
constexpr uint32_t CHUNK_COUNT = 12;
constexpr uint32_t SD_HZ = 4000000;
constexpr uint8_t SD_MAX_FILES = 12;
constexpr uint32_t DMA_FRAME_BYTES = 4096;
constexpr uint32_t DMA_POOL_BYTES = 49152;
constexpr uint32_t ADC_TASK_STACK_BYTES = 4096;
constexpr uint64_t SEGMENT_LIMIT = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t CHECKPOINT_CHUNKS = 64;
constexpr uint32_t INDEX_STRIDE_CHUNKS = 256;
constexpr size_t FIELD_SITE_MAX_BYTES = 64;
constexpr size_t FIELD_BUS_MAX_BYTES = 64;
constexpr size_t FIELD_POINT_MAX_BYTES = 64;
constexpr size_t FIELD_NOTE_MAX_BYTES = 160;
constexpr uint64_t FIELD_SD_MARGIN_BYTES = 16ULL * 1024ULL * 1024ULL;
constexpr const char *FIELD_CAMPAIGN_SCHEMA = "knx-field-campaign-1.0";
#ifndef S3_WRITER_PRIORITY
#define S3_WRITER_PRIORITY 2
#endif
#ifndef S3_WRITER_CORE
#define S3_WRITER_CORE 0
#endif

enum class State : uint8_t { IDLE, RUNNING, STOPPING, CLOSED, FAILED };
enum class StorageState : uint8_t { HEALTHY, OUTAGE, RECOVERING };
enum class CaptureMode : uint8_t { EVENT, CONTINUOUS_RAW };
enum class TimeSource : uint8_t { NONE, BROWSER, NTP };
enum class ChunkState : uint8_t { FREE, FILLING, HISTORY, PENDING, WRITING };
enum class AdcMode : uint8_t { OFF, OBSERVING, CAPTURING };
struct Chunk {
  uint64_t seq = 0, sampleStart = 0, segmentOffset = 0;
  uint32_t crc = 0, count = 0;
  ChunkState state = ChunkState::FREE;
  bool selected = false, stored = false, accounted = false;
  uint16_t data[CHUNK_SAMPLES];
};

SPIClass sdSpi(FSPI);
Chunk *chunks[CHUNK_COUNT]{};
QueueHandle_t readyQ = nullptr;
TaskHandle_t producerHandle = nullptr, writerHandle = nullptr;
File rawFile, mapFile, segmentsFile, indexFile, checkpointsFile, incidentsFile, eventsFile, gapsFile, transitionsFile;
String sessionDir;
std::atomic<State> state{State::IDLE};
std::atomic<StorageState> storageState{StorageState::HEALTHY};
std::atomic<bool> stopRequested{false}, producerDrained{true}, writerActive{false};
bool fatalConfiguration = false;
std::atomic<uint64_t> producedSamples{0}, producedChunks{0}, rawBytes{0}, lostSamples{0};
std::atomic<uint64_t> ignoredSamples{0}, storedSamples{0};
std::atomic<uint32_t> poolExhaustion{0}, sdErrors{0}, r1Count{0}, rawHandleFailures{0}, retryFailures{0}, reopenFailures{0};
std::atomic<uint32_t> faultInjected{0}, syntheticR3Count{0}, gapCount{0};
std::atomic<uint32_t> storageOutageCount{0}, storageRecoveryCount{0}, lostChunksTotal{0};
std::atomic<uint32_t> storageRecoveryAttempts{0}, storageRecoveryFailures{0}, storageRecoverySuccesses{0};
std::atomic<uint32_t> syntheticRecoveryFailures{0};
const char *lastSdErrorSource = "none";
const char *completionStatus = "FAILED";
uint64_t faultR3OnceChunk = 0;
bool faultR3OnceConsumed = false;
uint64_t faultOutageStartChunk = 0;
uint32_t faultOutageDurationMs = 0;
bool faultOutageConsumed = false;
uint64_t faultRecoveryStartChunk = 0;
uint32_t faultRecoveryFailuresRequested = 0;
bool faultRecoveryConsumed = false;
uint64_t gapSampleStart = 0, gapSampleEnd = 0;
uint32_t gapSegmentBefore = 0, gapSegmentAfter = 0;
uint64_t gapPreviousStoredEnd = 0, gapResumeSampleStart = 0;
uint64_t outageStartedUs = 0, outageEndedUs = 0;
uint64_t firstLostChunkSeq = 0, lastLostChunkSeq = 0;
uint64_t longestGapUs = 0, cumulativeStorageOutageUs = 0;
uint64_t nextRecoveryDueUs = 0;
uint32_t currentRecoveryBackoffMs = 0, maxRecoveryBackoffMs = 0;
uint64_t firstRawFailureUs = 0;
uint64_t startedUs = 0, closedUs = 0, adcCaptureStartedUs = 0, adcCaptureEndedUs = 0;
std::atomic<uint64_t> adcLastProducedUs{0};
uint64_t segmentBytes = 0, segmentSampleStart = 0, lastRawDebugUs = 0;
TimeSource pendingTimeSource = TimeSource::NONE, activeTimeSource = TimeSource::NONE;
uint64_t pendingUnixMs = 0, pendingReferenceMonoUs = 0, startUnixMs = 0;
int32_t pendingTimezoneOffsetMin = 0, activeTimezoneOffsetMin = 0;
bool absoluteTimeValid = false;
char startUtc[32] = {};
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
adc_continuous_handle_t adcHandle = nullptr;
uint8_t *adcDmaBuffer = nullptr;
std::atomic<AdcMode> adcMode{AdcMode::OFF};
std::atomic<uint32_t> dmaOverflows{0}, adcReadErrors{0};
std::atomic<uint32_t> adcTaskStackMinFree{UINT32_MAX};
uint64_t firstDmaOverflowUs = 0, firstDmaOverflowSample = 0;
uint64_t firstPoolExhaustionUs = 0, firstPoolExhaustionSample = 0;
uint32_t firstPoolFree = 0, firstPoolFilling = 0, firstPoolHistory = 0, firstPoolPending = 0, firstPoolWriting = 0;
std::atomic<uint32_t> chunkLifecycleErrors{0}, releasedSelectedChunks{0};
CaptureMode captureMode = CaptureMode::EVENT;
CaptureMode pendingCaptureMode = CaptureMode::EVENT;
String pendingSiteLabel, pendingBusLabel, pendingMeasurementPoint, pendingOperatorNote;
DetectionConfig detection{4095, 8333, 8333, true, "UNCALIBRATED_SAFE"};
D44ActivityDetector detector;
D44ActivityDetector calibrationDetector;
autocal::Calibration calibration;
Chunk *currentChunk = nullptr;
bool eventActive = false;
uint64_t eventStart = 0, eventTrigger = 0, eventEnd = 0, eventCount = 0, excursionCount = 0;
uint32_t eventTriggerD44 = 0, eventMaxD44 = 0;
uint16_t eventMin = UINT16_MAX, eventMax = 0;
uint16_t adcMin = UINT16_MAX, adcMax = 0;
uint64_t adcSum = 0;
uint64_t adcClipLow = 0, adcClipHigh = 0;
uint32_t d44Max = 0;
uint64_t requestedDurationUs = 60000000ULL;
bool preWifiHeapOk = false;
uint32_t preWifiInternalFree = 0, preWifiInternalMin = 0, preWifiInternalLargest = 0;
uint32_t preWifiDmaFree = 0, preWifiDmaMin = 0, preWifiDmaLargest = 0;
uint32_t captureInternalMin = UINT32_MAX, captureDmaMin = UINT32_MAX;
uint32_t captureInternalLargestMin = UINT32_MAX, captureDmaLargestMin = UINT32_MAX;
mbedtls_sha256_context rawShaContext;
bool rawShaActive = false;
uint8_t rawSha256[32]{};

const char *captureModeName(CaptureMode mode) {
  return mode == CaptureMode::CONTINUOUS_RAW ? "CONTINUOUS_RAW" : "EVENT";
}

String jsonStringOrNull(const String &value) {
  if (value.isEmpty()) return "null";
  String out = "\"";
  for (char c : value) {
    if (c == '\\' || c == '"') { out += '\\'; out += c; }
    else if (uint8_t(c) >= 0x20) out += c;
  }
  out += '"'; return out;
}

String fieldCampaignJson() {
  return "{\"schema\":\"" + String(FIELD_CAMPAIGN_SCHEMA) + "\",\"site\":" +
    jsonStringOrNull(pendingSiteLabel) + ",\"bus\":" + jsonStringOrNull(pendingBusLabel) +
    ",\"point\":" + jsonStringOrNull(pendingMeasurementPoint) + ",\"note\":" +
    jsonStringOrNull(pendingOperatorNote) + ",\"requested_duration_s\":" +
    String(requestedDurationUs / 1000000ULL) + "}";
}

void updateCaptureMemoryMinima() {
  const uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t dmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  const uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  const uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  captureInternalMin = min(captureInternalMin, internalFree); captureDmaMin = min(captureDmaMin, dmaFree);
  captureInternalLargestMin = min(captureInternalLargestMin, internalLargest);
  captureDmaLargestMin = min(captureDmaLargestMin, dmaLargest);
}

void updateQueueStats() {
  uint32_t freeDepth = 0;
  const uint64_t now = producedSamples.load();
  for (Chunk *chunk : chunks)
    if (chunk && (chunk->state == ChunkState::FREE ||
        (chunk->state == ChunkState::HISTORY && !chunk->selected &&
         (captureMode == CaptureMode::CONTINUOUS_RAW ||
         chunk->sampleStart + chunk->count + detection.preSamples <= now)))) ++freeDepth;
  const uint32_t readyDepth = readyQ ? uxQueueMessagesWaiting(readyQ) : 0;
  const uint32_t pending = readyDepth + (writerActive.load() ? 1U : 0U);
  if (freeDepth < freeMin) freeMin = freeDepth;
  if (readyDepth > readyMax) readyMax = readyDepth;
  if (pending > pendingMax) pendingMax = pending;
}

void countChunkStates(uint32_t &freeCount, uint32_t &fillingCount, uint32_t &historyCount,
                      uint32_t &pendingCount, uint32_t &writingCount) {
  freeCount = fillingCount = historyCount = pendingCount = writingCount = 0;
  for (Chunk *chunk : chunks) {
    if (!chunk) continue;
    switch (chunk->state) {
      case ChunkState::FREE: ++freeCount; break;
      case ChunkState::FILLING: ++fillingCount; break;
      case ChunkState::HISTORY: ++historyCount; break;
      case ChunkState::PENDING: ++pendingCount; break;
      case ChunkState::WRITING: ++writingCount; break;
    }
  }
}

void recordPoolExhaustion(uint64_t sampleIndex) {
  if (firstPoolExhaustionUs) return;
  firstPoolExhaustionUs = esp_timer_get_time() - startedUs;
  firstPoolExhaustionSample = sampleIndex;
  countChunkStates(firstPoolFree, firstPoolFilling, firstPoolHistory, firstPoolPending, firstPoolWriting);
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

const char *storageStateName(StorageState value) {
  switch (value) {
    case StorageState::HEALTHY: return "HEALTHY";
    case StorageState::OUTAGE: return "OUTAGE";
    default: return "RECOVERING";
  }
}

#if S3_NETWORK_ENABLED
namespace s3net {
bool stopForCapture();
}
#endif

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
      chunk.seq, chunk.sampleStart, chunk.sampleStart + chunk.count, chunk.count,
      segmentIndex, chunk.segmentOffset, chunk.count * sizeof(uint16_t), chunk.crc);
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

bool discardOutageChunk(Chunk &chunk) {
  if (!firstLostChunkSeq) {
    firstLostChunkSeq = chunk.seq;
    gapSampleStart = chunk.sampleStart;
  }
  lastLostChunkSeq = chunk.seq;
  gapSampleEnd = chunk.sampleStart + CHUNK_SAMPLES;
  ++lostChunksTotal;
  lostSamples += CHUNK_SAMPLES;
  return true;
}

bool logStorageTransition(const char *from, const char *to, const Chunk &chunk, const char *reason,
                          uint32_t attempt, const char *result, uint32_t backoffMs) {
  const uint64_t nowUs = esp_timer_get_time() - startedUs;
  const bool ok = transitionsFile.printf(
      "{\"record_type\":\"STORAGE_TRANSITION\",\"time_us\":\"%llu\",\"sample\":\"%llu\","
      "\"chunk_seq\":\"%llu\",\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\","
      "\"attempt\":%u,\"result\":\"%s\",\"backoff_ms\":%u}\n",
      nowUs, chunk.sampleStart, chunk.seq, from, to, reason, attempt, result, backoffMs) > 0;
  transitionsFile.flush();
  Serial.printf("{\"type\":\"STORAGE_TRANSITION\",\"time_us\":\"%llu\",\"sample\":\"%llu\","
                "\"chunk_seq\":\"%llu\",\"from\":\"%s\",\"to\":\"%s\",\"reason\":\"%s\","
                "\"attempt\":%u,\"result\":\"%s\",\"backoff_ms\":%u}\n",
                nowUs, chunk.sampleStart, chunk.seq, from, to, reason, attempt, result, backoffMs);
  return ok;
}

bool beginInjectedOutage(Chunk &chunk) {
  if (!abandonSegment("INJECTED_STORAGE_OUTAGE")) {
    failSd("segment_abandon");
    return false;
  }
  faultOutageConsumed = true;
  ++faultInjected;
  ++storageOutageCount;
  gapSegmentBefore = segmentIndex;
  gapSegmentAfter = segmentIndex + 1;
  gapPreviousStoredEnd = lastStoredSampleEnd;
  outageStartedUs = esp_timer_get_time() - startedUs;
  storageState = StorageState::OUTAGE;
  if (!logStorageTransition("HEALTHY", "OUTAGE", chunk, "INJECTED_STORAGE_OUTAGE", 0, "started",
                            faultOutageDurationMs)) {
    failSd("storage_transition");
    return false;
  }
  Serial.printf("{\"type\":\"STORAGE_OUTAGE_START\",\"chunk_seq\":\"%llu\","
                "\"sample_start\":\"%llu\",\"time_start_us\":\"%llu\",\"duration_ms\":%u,"
                "\"segment_before\":%u}\n",
                chunk.seq, chunk.sampleStart, outageStartedUs, faultOutageDurationMs, gapSegmentBefore);
  return discardOutageChunk(chunk);
}

bool beginInjectedRecoveryOutage(Chunk &chunk) {
  if (!abandonSegment("INJECTED_RECOVERY_OUTAGE")) {
    failSd("segment_abandon");
    return false;
  }
  faultRecoveryConsumed = true;
  ++faultInjected;
  ++storageOutageCount;
  gapSegmentBefore = segmentIndex;
  gapSegmentAfter = segmentIndex + 1;
  gapPreviousStoredEnd = lastStoredSampleEnd;
  outageStartedUs = esp_timer_get_time() - startedUs;
  currentRecoveryBackoffMs = 500;
  maxRecoveryBackoffMs = 500;
  nextRecoveryDueUs = outageStartedUs + uint64_t(currentRecoveryBackoffMs) * 1000ULL;
  storageState = StorageState::OUTAGE;
  if (!logStorageTransition("HEALTHY", "OUTAGE", chunk, "INJECTED_RECOVERY_OUTAGE", 0, "started",
                            currentRecoveryBackoffMs)) {
    failSd("storage_transition");
    return false;
  }
  return discardOutageChunk(chunk);
}

bool recoverInjectedOutage(Chunk &resumeChunk, const char *cause, uint32_t attempts, uint32_t failures) {
  storageState = StorageState::RECOVERING;
  if (!mapFile || !segmentsFile || !indexFile || !checkpointsFile || !incidentsFile || !eventsFile || !gapsFile ||
      !transitionsFile || !flushMap()) {
    failSd("outage_recovery_files");
    return false;
  }
  segmentIndex = gapSegmentAfter;
  segmentBytes = segmentChunks = segmentCrc = 0;
  if (!openSegment(resumeChunk.sampleStart)) {
    failSd("outage_recovery_segment");
    return false;
  }
  outageEndedUs = esp_timer_get_time() - startedUs;
  const uint64_t durationUs = outageEndedUs - outageStartedUs;
  if (!gapsFile.printf(
      "{\"record_type\":\"GAP\",\"gap_id\":\"1\",\"kind\":\"STORAGE_OUTAGE\","
      "\"cause\":\"%s\",\"sample_start\":\"%llu\",\"sample_end\":\"%llu\","
      "\"lost_raw_samples\":\"%llu\",\"time_start_us\":\"%llu\",\"time_end_us\":\"%llu\","
      "\"duration_us\":\"%llu\",\"first_lost_chunk_seq\":\"%llu\","
      "\"last_lost_chunk_seq\":\"%llu\",\"lost_chunk_count\":\"%u\","
      "\"segment_before\":%u,\"segment_after\":%u,\"recovered\":true,"
      "\"recovery_attempts\":%u,\"failed_recovery_attempts\":%u}\n",
      cause, gapSampleStart, gapSampleEnd, lostSamples.load(), outageStartedUs, outageEndedUs, durationUs,
      firstLostChunkSeq, lastLostChunkSeq, lostChunksTotal.load(), gapSegmentBefore, gapSegmentAfter,
      attempts, failures)) {
    failSd("gap");
    return false;
  }
  gapsFile.flush();
  incidentsFile.printf(
      "{\"incident\":\"SYNTHETIC-OUTAGE-1\",\"time_start_us\":\"%llu\",\"time_end_us\":\"%llu\","
      "\"first_lost_chunk_seq\":\"%llu\",\"last_lost_chunk_seq\":\"%llu\","
      "\"lost_chunk_count\":\"%u\",\"result\":\"RECOVERED\",\"physical_io_attempted\":false}\n",
      outageStartedUs, outageEndedUs, firstLostChunkSeq, lastLostChunkSeq, lostChunksTotal.load());
  incidentsFile.flush();
  ++gapCount;
  ++storageRecoveryCount;
  ++storageRecoverySuccesses;
  cumulativeStorageOutageUs += durationUs;
  if (durationUs > longestGapUs) longestGapUs = durationUs;
  gapResumeSampleStart = resumeChunk.sampleStart;
  if (!logStorageTransition("RECOVERING", "HEALTHY", resumeChunk, cause, attempts, "success", 0)) {
    failSd("storage_transition");
    return false;
  }
  storageState = StorageState::HEALTHY;
  currentRecoveryBackoffMs = 0;
  Serial.printf("{\"type\":\"STORAGE_OUTAGE_RECOVERED\",\"resume_chunk_seq\":\"%llu\","
                "\"resume_sample_start\":\"%llu\",\"time_end_us\":\"%llu\",\"duration_us\":\"%llu\","
                "\"lost_chunk_count\":%u,\"lost_raw_samples\":\"%llu\",\"segment_after\":%u}\n",
                resumeChunk.seq, resumeChunk.sampleStart, outageEndedUs, durationUs, lostChunksTotal.load(),
                lostSamples.load(), gapSegmentAfter);
  return true;
}

bool attemptInjectedRecovery(Chunk &chunk) {
  const uint32_t attempt = ++storageRecoveryAttempts;
  storageState = StorageState::RECOVERING;
  if (!logStorageTransition("OUTAGE", "RECOVERING", chunk, "INJECTED_RECOVERY_OUTAGE", attempt,
                            "attempting", currentRecoveryBackoffMs)) {
    failSd("storage_transition");
    return false;
  }
  if (attempt <= faultRecoveryFailuresRequested) {
    ++storageRecoveryFailures;
    ++syntheticRecoveryFailures;
    const uint32_t doubledBackoff = currentRecoveryBackoffMs * 2U;
    const uint32_t nextBackoff = doubledBackoff < 4000U ? doubledBackoff : 4000U;
    if (nextBackoff > maxRecoveryBackoffMs) maxRecoveryBackoffMs = nextBackoff;
    if (!logStorageTransition("RECOVERING", "OUTAGE", chunk, "INJECTED_RECOVERY_OUTAGE", attempt,
                              "synthetic_failure", nextBackoff)) {
      failSd("storage_transition");
      return false;
    }
    storageState = StorageState::OUTAGE;
    currentRecoveryBackoffMs = nextBackoff;
    nextRecoveryDueUs = (esp_timer_get_time() - startedUs) + uint64_t(nextBackoff) * 1000ULL;
    return discardOutageChunk(chunk);
  }
  return recoverInjectedOutage(chunk, "INJECTED_RECOVERY_OUTAGE", attempt, storageRecoveryFailures.load());
}

bool writeChunk(Chunk &chunk) {
  const uint32_t chunkBytes = chunk.count * sizeof(uint16_t);
  if (chunk.stored || (storedChunks && chunk.sampleStart < lastStoredSampleEnd)) {
    ++chunkLifecycleErrors;
    failSd("chunk_lifecycle");
    return false;
  }
  if (storageState.load() == StorageState::OUTAGE) {
    const uint64_t nowUs = esp_timer_get_time() - startedUs;
    if (faultRecoveryConsumed) {
      if (nowUs < nextRecoveryDueUs) return discardOutageChunk(chunk);
      if (!attemptInjectedRecovery(chunk)) return false;
      if (storageState.load() == StorageState::OUTAGE) return true;
    } else {
      if (nowUs - outageStartedUs < uint64_t(faultOutageDurationMs) * 1000ULL)
        return discardOutageChunk(chunk);
      ++storageRecoveryAttempts;
      if (!logStorageTransition("OUTAGE", "RECOVERING", chunk, "INJECTED_STORAGE_OUTAGE", 1,
                                "attempting", faultOutageDurationMs)) {
        failSd("storage_transition");
        return false;
      }
      if (!recoverInjectedOutage(chunk, "INJECTED_STORAGE_OUTAGE", 1, 0)) return false;
    }
  }
  if (faultRecoveryStartChunk && !faultRecoveryConsumed && chunk.seq == faultRecoveryStartChunk)
    return beginInjectedRecoveryOutage(chunk);
  if (faultOutageStartChunk && !faultOutageConsumed && chunk.seq == faultOutageStartChunk)
    return beginInjectedOutage(chunk);
  if (!prepareSegment(chunk.sampleStart)) { failSd(rawFile ? "other_segment_metadata" : "raw_initial"); return false; }
  if (faultR3OnceChunk && !faultR3OnceConsumed && chunk.seq == faultR3OnceChunk)
    return injectR3Gap(chunk);
  chunk.segmentOffset = segmentBytes;
  errno = 0;
  const uint64_t t0 = esp_timer_get_time();
  size_t written = rawFile.write(reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
  const uint32_t latencyUs = uint32_t(esp_timer_get_time() - t0);
  writeLatencyTotalUs += latencyUs;
  ++writeCount;
  if (latencyUs < writeLatencyMinUs) writeLatencyMinUs = latencyUs;
  if (latencyUs > writeLatencyMaxUs) {
    writeLatencyMaxUs = latencyUs;
    writeLatencyMaxChunk = uint32_t(chunk.seq);
  }
  const int firstErrno = errno;
  if (written != chunkBytes) {
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
      retryWritten = rawFile.write(reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
      retryErrno = errno;
    }
    const char *result = !rawFile ? "R4" : !coherent ? "R2" : retryWritten == chunkBytes ? "R1" : "R3";
    incidentsFile.printf(
        "{\"incident\":%u,\"time_us\":\"%llu\",\"sample_index\":\"%llu\","
        "\"chunk_start\":\"%llu\",\"errno\":%d,\"requested\":%u,\"returned\":%u,"
        "\"latency_us\":%u,\"reopen_errno\":%d,\"reopen_size\":\"%llu\","
        "\"retry_returned\":%u,\"retry_errno\":%d,\"result\":\"%s\"}\n",
        rawHandleFailures.load(), esp_timer_get_time() - startedUs, producedSamples.load(),
        chunk.sampleStart, firstErrno, chunkBytes, unsigned(written), latencyUs,
        reopenErrno, reopenSize, unsigned(retryWritten), retryErrno, result);
    incidentsFile.flush();
    if (retryWritten == chunkBytes) { ++r1Count; written = retryWritten; }
    else { ++retryFailures; lostSamples += chunk.count - written / 2; failSd(!rawFile ? "reopen" : "raw_retry"); return false; }
  }
  chunk.crc = esp_crc32_le(0, reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
  if (rawShaActive) mbedtls_sha256_update(&rawShaContext, reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
  sessionCrc = esp_crc32_le(sessionCrc, reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
  segmentCrc = esp_crc32_le(segmentCrc, reinterpret_cast<uint8_t *>(chunk.data), chunkBytes);
  if ((storedChunks % INDEX_STRIDE_CHUNKS) == 0) {
    indexFile.printf("{\"record_type\":\"CHUNK_INDEX\",\"chunk_ordinal\":\"%llu\","
                     "\"sample_start\":\"%llu\",\"segment_index\":%u,\"segment_offset\":\"%llu\"}\n",
                     storedChunks, chunk.sampleStart, segmentIndex, chunk.segmentOffset);
  }
  if (!appendMap(chunk)) { failSd("map"); return false; }
  rawBytes += chunkBytes; storedSamples += chunk.count;
  segmentBytes += chunkBytes;
  ++segmentChunks; ++storedChunks;
  lastStoredSampleEnd = chunk.sampleStart + chunk.count;
  if (gapCount.load() && !gapResumeSampleStart && chunk.sampleStart >= gapSampleEnd)
    gapResumeSampleStart = chunk.sampleStart;
  if (!writeCheckpoint()) { failSd("checkpoint"); return false; }
  return true;
}

bool IRAM_ATTR onAdcOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  ++dmaOverflows;
  return false;
}

bool initAdc() {
  adc_continuous_handle_cfg_t handleConfig{};
  handleConfig.max_store_buf_size = DMA_POOL_BYTES;
  handleConfig.conv_frame_size = DMA_FRAME_BYTES;
  if (adc_continuous_new_handle(&handleConfig, &adcHandle) != ESP_OK) return false;
  adc_digi_pattern_config_t pattern{};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = board::ADC_CHANNEL;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t config{};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = SAMPLE_RATE;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if (adc_continuous_config(adcHandle, &config) != ESP_OK) return false;
  adc_continuous_evt_cbs_t callbacks{};
  callbacks.on_pool_ovf = onAdcOverflow;
  return adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr) == ESP_OK;
}

bool startIdleObservation() {
  if (!adcHandle || state.load() == State::RUNNING || state.load() == State::STOPPING) return false;
  if (adcMode.load() == AdcMode::OBSERVING) return true;
  calibrationDetector.reset();
  if (adc_continuous_start(adcHandle) != ESP_OK) return false;
  adcMode = AdcMode::OBSERVING;
  return true;
}

void stopIdleObservation() {
  if (adcMode.load() != AdcMode::OBSERVING) return;
  adcMode = AdcMode::OFF;
  if (adcHandle) adc_continuous_stop(adcHandle);
  const uint32_t deadline = millis() + 100;
  while (!producerDrained.load() && int32_t(deadline - millis()) > 0) delay(1);
}

Chunk *acquireChunk(uint64_t sampleStart) {
  Chunk *candidate = nullptr;
  for (Chunk *chunk : chunks) {
    if (chunk->state == ChunkState::FREE) { candidate = chunk; break; }
    if (chunk->state == ChunkState::HISTORY && !chunk->selected &&
        (captureMode == CaptureMode::CONTINUOUS_RAW ||
         chunk->sampleStart + chunk->count + detection.preSamples <= sampleStart)) {
      if (!candidate || chunk->sampleStart < candidate->sampleStart) candidate = chunk;
    }
  }
  if (!candidate) return nullptr;
  if (candidate->state == ChunkState::HISTORY && !candidate->accounted) ignoredSamples += candidate->count;
  candidate->seq = producedChunks.load() + 1;
  candidate->sampleStart = sampleStart;
  candidate->segmentOffset = candidate->crc = candidate->count = 0;
  candidate->selected = captureMode == CaptureMode::CONTINUOUS_RAW;
  candidate->stored = candidate->accounted = false;
  candidate->state = ChunkState::FILLING;
  return candidate;
}

void enqueueSelected(Chunk *chunk) {
  if (!chunk || chunk->state != ChunkState::HISTORY || !chunk->selected || chunk->stored) return;
  chunk->state = ChunkState::PENDING;
  if (xQueueSend(readyQ, &chunk, 0) != pdTRUE) {
    recordPoolExhaustion(chunk->sampleStart);
    ++poolExhaustion; lostSamples += chunk->count; chunk->accounted = true;
    state = State::FAILED; stopRequested = true;
  }
  updateQueueStats();
}

void selectRange(uint64_t from, uint64_t to) {
  Chunk *history[CHUNK_COUNT]{};
  uint32_t historyCount = 0;
  for (Chunk *chunk : chunks) {
    const uint64_t end = chunk->sampleStart + chunk->count;
    if (chunk->state != ChunkState::FREE && !chunk->stored && end > from && chunk->sampleStart < to) {
      chunk->selected = true;
      if (chunk->state == ChunkState::HISTORY) history[historyCount++] = chunk;
    }
  }
  for (uint32_t i = 1; i < historyCount; ++i) {
    Chunk *value = history[i]; uint32_t j = i;
    while (j && history[j - 1]->sampleStart > value->sampleStart) { history[j] = history[j - 1]; --j; }
    history[j] = value;
  }
  for (uint32_t i = 0; i < historyCount; ++i) enqueueSelected(history[i]);
}

void closeEvent(uint64_t closeSample, bool truncated) {
  eventsFile.printf("{\"record_type\":\"EVENT\",\"event_id\":\"%llu\",\"sample_start\":\"%llu\"," 
                    "\"trigger_sample\":\"%llu\",\"sample_end\":\"%llu\",\"duration_samples\":\"%llu\"," 
                    "\"trigger_d44\":%u,\"max_d44\":%u,\"adc_min\":%u,\"adc_max\":%u," 
                    "\"excursions\":\"%llu\",\"truncated\":%s}\n",
                    eventCount, eventStart, eventTrigger, closeSample, closeSample - eventStart,
                    eventTriggerD44, eventMaxD44, eventMin, eventMax, excursionCount,
                    truncated ? "true" : "false");
  eventActive = false;
}

void finalizeCurrentChunk() {
  if (!currentChunk) return;
  currentChunk->state = ChunkState::HISTORY;
  ++producedChunks;
  if (currentChunk->selected) enqueueSelected(currentChunk);
  updateCaptureMemoryMinima();
  currentChunk = nullptr;
}

void producerTask(void *) {
  for (;;) {
    const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
    uint32_t observed = adcTaskStackMinFree.load();
    while (stackFree < observed && !adcTaskStackMinFree.compare_exchange_weak(observed, stackFree)) {}
    if (dmaOverflows.load() && !firstDmaOverflowUs) {
      firstDmaOverflowUs = esp_timer_get_time() - startedUs;
      firstDmaOverflowSample = producedSamples.load();
    }
    const AdcMode mode = adcMode.load();
    if (mode == AdcMode::OFF || (mode == AdcMode::CAPTURING && stopRequested.load())) {
      producerDrained = true;
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    producerDrained = false;
    uint32_t bytes = 0;
    const esp_err_t error = adc_continuous_read(adcHandle, adcDmaBuffer, DMA_FRAME_BYTES, &bytes, 50);
    if (error == ESP_ERR_TIMEOUT) continue;
    if (error != ESP_OK) { ++adcReadErrors; continue; }
    for (uint32_t offset = 0; offset + SOC_ADC_DIGI_RESULT_BYTES <= bytes;
         offset += SOC_ADC_DIGI_RESULT_BYTES) {
      const auto *result = reinterpret_cast<const adc_digi_output_data_t *>(adcDmaBuffer + offset);
      if (result->type2.channel != board::ADC_CHANNEL) continue;
      const uint16_t value = result->type2.data;
      if (mode == AdcMode::OBSERVING) {
        const uint32_t d44 = calibrationDetector.push(value);
        calibration.process(value, d44);
        continue;
      }
      const uint64_t index = producedSamples.fetch_add(1);
      if (!currentChunk) currentChunk = acquireChunk(index);
      if (!currentChunk) {
        recordPoolExhaustion(index);
        ++poolExhaustion; ++lostSamples; state = State::FAILED; stopRequested = true; break;
      }
      currentChunk->data[currentChunk->count++] = value;
      if (eventActive) currentChunk->selected = true;
      adcSum += value; if (value < adcMin) adcMin = value; if (value > adcMax) adcMax = value;
      if (value <= 16) ++adcClipLow; if (value >= 4079) ++adcClipHigh;
      const uint32_t d44 = captureMode == CaptureMode::EVENT ? detector.push(value) : 0;
      if (d44 > d44Max) d44Max = d44;
      if (captureMode == CaptureMode::EVENT && detection.enabled && d44 > detection.d44Threshold) {
        ++excursionCount;
        if (!eventActive) {
          eventActive = true; ++eventCount; eventTrigger = index;
          eventStart = index > detection.preSamples ? index - detection.preSamples : 0;
          eventTriggerD44 = eventMaxD44 = d44; eventMin = eventMax = value;
          selectRange(eventStart, index + 1);
        } else if (d44 > eventMaxD44) eventMaxD44 = d44;
        eventEnd = index + detection.postSamples;
      }
      if (eventActive) {
        if (value < eventMin) eventMin = value; if (value > eventMax) eventMax = value;
        if (d44 > eventMaxD44) eventMaxD44 = d44;
        if (index >= eventEnd) closeEvent(eventEnd, false);
      }
      if (currentChunk->count == CHUNK_SAMPLES) finalizeCurrentChunk();
      if (stopRequested.load()) break;
    }
    if (mode == AdcMode::CAPTURING && bytes) adcLastProducedUs = esp_timer_get_time();
  }
}

void writerTask(void *) {
  for (;;) {
    Chunk *chunk = nullptr;
    if (xQueueReceive(readyQ, &chunk, pdMS_TO_TICKS(20)) == pdTRUE) {
      const uint64_t holdStarted = esp_timer_get_time();
      writerActive = true;
      if (chunk->state != ChunkState::PENDING || chunk->stored) ++chunkLifecycleErrors;
      chunk->state = ChunkState::WRITING;
      updateQueueStats();
      const bool handled = writeChunk(*chunk);
      if (handled) {
        chunk->stored = true; chunk->accounted = true; chunk->selected = false;
        ++releasedSelectedChunks;
      }
      chunk->state = ChunkState::HISTORY;
      writerActive = false;
      const uint32_t heldUs = uint32_t(esp_timer_get_time() - holdStarted);
      if (heldUs > writerHoldMaxUs) { writerHoldMaxUs = heldUs; writerHoldMaxChunk = uint32_t(chunk->seq); }
      updateQueueStats();
    }
  }
}

void resetCounters() {
  producedSamples = producedChunks = rawBytes = lostSamples = ignoredSamples = storedSamples = 0;
  poolExhaustion = sdErrors = r1Count = rawHandleFailures = retryFailures = reopenFailures = 0;
  faultInjected = syntheticR3Count = gapCount = 0;
  storageOutageCount = storageRecoveryCount = lostChunksTotal = 0;
  storageRecoveryAttempts = storageRecoveryFailures = storageRecoverySuccesses = syntheticRecoveryFailures = 0;
  storageState = StorageState::HEALTHY;
  lastSdErrorSource = "none";
  completionStatus = "FAILED";
  faultR3OnceConsumed = false;
  faultOutageConsumed = false;
  faultRecoveryConsumed = false;
  gapSampleStart = gapSampleEnd = 0;
  gapSegmentBefore = gapSegmentAfter = 0;
  gapPreviousStoredEnd = gapResumeSampleStart = 0;
  outageStartedUs = outageEndedUs = 0;
  firstLostChunkSeq = lastLostChunkSeq = 0;
  longestGapUs = cumulativeStorageOutageUs = 0;
  nextRecoveryDueUs = 0;
  currentRecoveryBackoffMs = maxRecoveryBackoffMs = 0;
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
  dmaOverflows = adcReadErrors = 0;
  firstDmaOverflowUs = firstDmaOverflowSample = 0;
  firstPoolExhaustionUs = firstPoolExhaustionSample = 0;
  firstPoolFree = firstPoolFilling = firstPoolHistory = firstPoolPending = firstPoolWriting = 0;
  chunkLifecycleErrors = releasedSelectedChunks = 0;
  adcMin = UINT16_MAX; adcMax = d44Max = 0; adcSum = 0;
  adcClipLow = adcClipHigh = 0;
  eventActive = false; eventStart = eventTrigger = eventEnd = eventCount = excursionCount = 0;
  eventTriggerD44 = eventMaxD44 = 0; eventMin = UINT16_MAX; eventMax = 0;
  detector.reset(); currentChunk = nullptr;
  captureInternalMin = captureDmaMin = captureInternalLargestMin = captureDmaLargestMin = UINT32_MAX;
  memset(rawSha256, 0, sizeof(rawSha256)); rawShaActive = false; lastRawDebugUs = 0;
  while (readyQ && uxQueueMessagesWaiting(readyQ)) { Chunk *discard = nullptr; xQueueReceive(readyQ, &discard, 0); }
  for (Chunk *chunk : chunks) if (chunk) { memset(chunk, 0, sizeof(Chunk)); chunk->state = ChunkState::FREE; }
}

const char *timeSourceName(TimeSource source) {
  switch (source) {
    case TimeSource::BROWSER: return "BROWSER";
    case TimeSource::NTP: return "NTP";
    default: return "NONE";
  }
}

void prepareTimeNone() {
  pendingTimeSource = TimeSource::NONE; pendingUnixMs = 0;
  pendingReferenceMonoUs = esp_timer_get_time(); pendingTimezoneOffsetMin = 0;
}

void prepareTimeBrowser(uint64_t unixMs, int32_t timezoneOffsetMin) {
  constexpr uint64_t MIN_VALID_UNIX_MS = 946684800000ULL;   // 2000-01-01 UTC
  constexpr uint64_t MAX_VALID_UNIX_MS = 4102444800000ULL;  // 2100-01-01 UTC
  if (unixMs < MIN_VALID_UNIX_MS || unixMs >= MAX_VALID_UNIX_MS) { prepareTimeNone(); return; }
  pendingTimeSource = TimeSource::BROWSER; pendingUnixMs = unixMs;
  pendingReferenceMonoUs = esp_timer_get_time(); pendingTimezoneOffsetMin = timezoneOffsetMin;
}

void activateTimeReference(uint64_t monotonicOriginUs) {
  activeTimeSource = pendingTimeSource;
  activeTimezoneOffsetMin = pendingTimezoneOffsetMin;
  absoluteTimeValid = activeTimeSource != TimeSource::NONE && pendingUnixMs != 0;
  startUnixMs = absoluteTimeValid ? pendingUnixMs + (monotonicOriginUs - pendingReferenceMonoUs) / 1000ULL : 0;
  startUtc[0] = 0;
  if (absoluteTimeValid) {
    const time_t seconds = time_t(startUnixMs / 1000ULL); struct tm utc{};
    if (gmtime_r(&seconds, &utc)) {
      char base[24]; strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &utc);
      snprintf(startUtc, sizeof(startUtc), "%s.%03uZ", base, unsigned(startUnixMs % 1000ULL));
    } else { absoluteTimeValid = false; activeTimeSource = TimeSource::NONE; startUnixMs = 0; }
  }
}

bool startRun() {
  if (!sdReady || storageState.load() != StorageState::HEALTHY ||
      (state.load() != State::IDLE && state.load() != State::CLOSED)) return false;
  if (captureMode == CaptureMode::EVENT && !calibration.valid()) return false;
  detection.d44Threshold = calibration.valid() ? calibration.validatedThreshold() : 0;
  detection.profile = calibration.valid() ? "AUTO_D44_V1" : "NONE";
  if (captureMode == CaptureMode::CONTINUOUS_RAW) {
    const uint64_t estimated = (requestedDurationUs / 1000000ULL + 1ULL) * SAMPLE_RATE * sizeof(uint16_t);
    const uint64_t freeBytes = SD.totalBytes() > SD.usedBytes() ? SD.totalBytes() - SD.usedBytes() : 0;
    if (freeBytes < estimated + FIELD_SD_MARGIN_BYTES) {
      Serial.printf("{\"type\":\"START_REJECTED\",\"reason\":\"SD_SPACE\",\"estimated_raw_bytes\":\"%llu\",\"free_bytes\":\"%llu\"}\n", estimated, freeBytes);
      return false;
    }
  }
  resetCounters();
  char name[32]; snprintf(name, sizeof(name), "/KNX-%08lX", (unsigned long)esp_random());
  sessionDir = name;
  if (!SD.mkdir(sessionDir)) return false;
  mapFile = SD.open(sessionDir + "/chunks.jsonl", FILE_WRITE);
  segmentsFile = SD.open(sessionDir + "/segments.jsonl", FILE_WRITE);
  indexFile = SD.open(sessionDir + "/chunk-index.jsonl", FILE_WRITE);
  checkpointsFile = SD.open(sessionDir + "/checkpoints.jsonl", FILE_WRITE);
  incidentsFile = SD.open(sessionDir + "/sd-incidents.jsonl", FILE_WRITE);
  eventsFile = SD.open(sessionDir + "/events.jsonl", FILE_WRITE);
  gapsFile = SD.open(sessionDir + "/gaps.jsonl", FILE_WRITE);
  transitionsFile = SD.open(sessionDir + "/storage-transitions.jsonl", FILE_WRITE);
  if (!mapFile || !segmentsFile || !indexFile || !checkpointsFile || !incidentsFile || !eventsFile || !gapsFile ||
      !transitionsFile) {
    state = State::FAILED; return false;
  }
  File session = SD.open(sessionDir + "/session-start.json", FILE_WRITE);
  if (!session) { state = State::FAILED; return false; }
  startedUs = esp_timer_get_time(); closedUs = adcCaptureStartedUs = adcCaptureEndedUs = 0;
  adcLastProducedUs = 0;
  mbedtls_sha256_init(&rawShaContext);
  mbedtls_sha256_starts(&rawShaContext, 0);
  rawShaActive = true;
  updateCaptureMemoryMinima();
  activateTimeReference(startedUs);
  String absoluteFields;
  const String campaignFields = fieldCampaignJson();
  if (absoluteTimeValid) {
    absoluteFields = "\"start_unix_ms\":" + String(startUnixMs) + ",\"start_utc\":\"" + String(startUtc) +
      "\",\"browser_timezone_offset_min\":" + String(activeTimezoneOffsetMin);
  } else {
    absoluteFields = "\"start_unix_ms\":null,\"start_utc\":null,\"browser_timezone_offset_min\":null";
  }
  session.printf("{\"schema_version\":\"knx-long-session-1.1\",\"session_id\":\"%s\",\"firmware\":\"%s\",\"analyzer_id\":\"S3-%08llX\","
                 "\"board_profile\":\"XIAO_ESP32S3\",\"frontend_profile\":\"KNX_DIVIDER_390K_27K\","
                 "\"acquisition_mode\":\"%s\",\"sample_rate_hz\":%u,\"sample_rate_configured_hz\":%u,"
                 "\"sample_type\":\"uint16_le\",\"chunk_samples\":%u,\"chunk_bytes\":%u,"
                 "\"capture_mode\":\"%s\",\"trigger_profile\":\"%s\",\"d44_threshold\":%u,"
                 "\"pre_samples\":%u,\"post_samples\":%u,\"adc_gpio\":1,\"adc_unit\":1,\"adc_channel\":0,"
                 "\"adc_resolution_bits\":12,\"adc_attenuation\":\"12dB\",\"endianness\":\"little\","
                 "\"calibration_state\":\"%s\","
                 "\"calibration_schema\":\"knx-calibration-1.0\",\"calibration_algorithm\":\"d44-autocal-v1\","
                 "\"calibration_id_crc32\":\"%08X\",\"calibration_confidence\":%u,\"calibration_confidence_category\":\"%s\","
                 "\"calibration_noise_upper\":%u,\"calibration_activity_p10\":%u,\"calibration_quiet_windows\":%u,"
                 "\"calibration_active_windows\":%u,\"calibration_independent_bursts\":%u,"
                 "\"calibration_board_profile\":\"XIAO_ESP32S3_GPIO1\","
                 "\"calibration_frontend_profile\":\"KNX_DIVIDER_390K_27K\","
                 "\"calibration_adc_profile\":\"ADC1_CH0_12DB_12BIT_83333HZ\","
                 "\"calibration_adc_zero\":\"%llu\",\"calibration_adc_low16\":\"%llu\","
                 "\"calibration_adc_high4079\":\"%llu\",\"calibration_adc_max4095\":\"%llu\","
                 "\"field_campaign\":%s,"
                 "\"calibrated_unix_ms\":null,\"time_schema_version\":\"1.0\","
                 "\"time_source\":\"%s\",\"absolute_time_valid\":%s,"
                 "%s,\"monotonic_origin_us\":\"%llu\",\"camera\":false,\"microphone\":false}\n",
                 sessionDir.substring(1).c_str(), FIRMWARE_VERSION, ESP.getEfuseMac(), captureModeName(captureMode),
                 SAMPLE_RATE, SAMPLE_RATE, CHUNK_SAMPLES, CHUNK_BYTES, captureModeName(captureMode),
                 detection.profile, detection.d44Threshold, detection.preSamples, detection.postSamples,
                 autocal::stateName(calibration.state()),
                 calibration.identityCrc(), calibration.confidence(), calibration.confidenceName(),
                 calibration.noiseUpper(), calibration.activityP10(), calibration.quietWindows(),
                 calibration.activeWindows(), calibration.bursts(), calibration.adcZero(), calibration.adcLow16(),
                 calibration.adcHigh4079(), calibration.adcMax4095(), campaignFields.c_str(),
                 timeSourceName(activeTimeSource), absoluteTimeValid ? "true" : "false",
                 absoluteFields.c_str(), startedUs);
  session.flush(); session.close();
  stopRequested = false; producerDrained = false;
  adcMode = AdcMode::CAPTURING;
  if (!adcHandle || adc_continuous_start(adcHandle) != ESP_OK) {
    adcMode = AdcMode::OFF; state = State::FAILED; return false;
  }
  adcCaptureStartedUs = esp_timer_get_time();
  state = State::RUNNING;
  Serial.printf("{\"type\":\"START\",\"dir\":\"%s\",\"source\":\"ADC_GPIO1\",\"mode\":\"%s\","
                "\"duration_us\":\"%llu\",\"sample_rate_target_hz\":%u}\n", sessionDir.c_str(),
                captureModeName(captureMode),
                requestedDurationUs, SAMPLE_RATE);
  return true;
}

void finalizeRun() {
  State before = state.load();
  if (before != State::RUNNING && before != State::FAILED) return;
  if (before == State::RUNNING) state = State::STOPPING;
  stopRequested = true;
  adcMode = AdcMode::OFF;
  if (adcHandle) adc_continuous_stop(adcHandle);
  const uint32_t drainDeadline = millis() + 3000;
  while (!producerDrained.load() && int32_t(drainDeadline - millis()) > 0) delay(1);
  adcCaptureEndedUs = adcLastProducedUs.load();
  if (adcCaptureEndedUs < adcCaptureStartedUs) adcCaptureEndedUs = adcCaptureStartedUs;
  if (eventActive) closeEvent(producedSamples.load(), true);
  finalizeCurrentChunk();
  for (Chunk *chunk : chunks) if (chunk->state == ChunkState::HISTORY && !chunk->selected && !chunk->accounted) {
    ignoredSamples += chunk->count; chunk->accounted = true;
  }
  while ((uxQueueMessagesWaiting(readyQ) || writerActive.load()) && int32_t(drainDeadline - millis()) > 0) delay(1);
  flushMap();
  completeSegment();
  mapFile.flush(); mapFile.close(); segmentsFile.close(); indexFile.flush(); indexFile.close();
  checkpointsFile.flush(); checkpointsFile.close(); incidentsFile.flush(); incidentsFile.close();
  eventsFile.flush(); eventsFile.close(); gapsFile.flush(); gapsFile.close();
  transitionsFile.flush(); transitionsFile.close();
  closedUs = esp_timer_get_time();
  if (rawShaActive) {
    mbedtls_sha256_finish(&rawShaContext, rawSha256);
    mbedtls_sha256_free(&rawShaContext); rawShaActive = false;
  }
  char rawShaHex[65];
  for (uint8_t i = 0; i < 32; ++i) snprintf(rawShaHex + i * 2, 3, "%02X", rawSha256[i]);
  rawShaHex[64] = 0;
  invariantOk = producedSamples.load() == storedSamples.load() + ignoredSamples.load() + lostSamples.load();
  const bool r3FaultExpected = faultR3OnceChunk != 0;
  const bool outageFaultExpected = faultOutageStartChunk != 0;
  const bool recoveryFaultExpected = faultRecoveryStartChunk != 0;
  const bool faultExpected = r3FaultExpected || outageFaultExpected || recoveryFaultExpected;
  const bool exactInjectedR3Gap = r3FaultExpected && !outageFaultExpected && faultR3OnceConsumed && faultInjected.load() == 1 &&
      syntheticR3Count.load() == 1 && gapCount.load() == 1 && lostSamples.load() == CHUNK_SAMPLES &&
      gapSampleEnd - gapSampleStart == CHUNK_SAMPLES && gapSegmentAfter == gapSegmentBefore + 1 &&
      gapPreviousStoredEnd == gapSampleStart && gapResumeSampleStart == gapSampleEnd;
  const bool exactInjectedOutage = outageFaultExpected && !r3FaultExpected && faultOutageConsumed && faultInjected.load() == 1 &&
      storageOutageCount.load() == 1 && storageRecoveryCount.load() == 1 && storageState.load() == StorageState::HEALTHY &&
      gapCount.load() == 1 && lostChunksTotal.load() > 0 && lostSamples.load() == uint64_t(lostChunksTotal.load()) * CHUNK_SAMPLES &&
      gapSampleEnd - gapSampleStart == lostSamples.load() && gapSegmentAfter == gapSegmentBefore + 1 &&
      gapPreviousStoredEnd == gapSampleStart && gapResumeSampleStart == gapSampleEnd;
  const bool exactInjectedRecovery = recoveryFaultExpected && !r3FaultExpected && !outageFaultExpected &&
      faultRecoveryConsumed && faultInjected.load() == 1 && storageOutageCount.load() == 1 &&
      storageRecoveryAttempts.load() == faultRecoveryFailuresRequested + 1 &&
      storageRecoveryFailures.load() == faultRecoveryFailuresRequested && syntheticRecoveryFailures.load() == faultRecoveryFailuresRequested &&
      storageRecoverySuccesses.load() == 1 && storageRecoveryCount.load() == 1 && storageState.load() == StorageState::HEALTHY &&
      gapCount.load() == 1 && lostChunksTotal.load() > 0 && lostSamples.load() == uint64_t(lostChunksTotal.load()) * CHUNK_SAMPLES &&
      gapSampleEnd - gapSampleStart == lostSamples.load() && gapSegmentAfter == gapSegmentBefore + 1 &&
      gapPreviousStoredEnd == gapSampleStart && gapResumeSampleStart == gapSampleEnd && maxRecoveryBackoffMs <= 4000;
  const bool structurallyHealthy = before != State::FAILED && sdErrors.load() == 0 && poolExhaustion.load() == 0 &&
      retryFailures.load() == 0 && dmaOverflows.load() == 0 && chunkLifecycleErrors.load() == 0 && invariantOk;
  finalPass = structurallyHealthy &&
      (faultExpected ? (exactInjectedR3Gap || exactInjectedOutage || exactInjectedRecovery) : lostSamples.load() == 0);
  if (!structurallyHealthy || !finalPass) completionStatus = "FAILED";
  else if (exactInjectedR3Gap || exactInjectedOutage || exactInjectedRecovery) completionStatus = "PARTIAL";
  else if (r1Count.load()) completionStatus = "COMPLETE_WITH_RECOVERED_ERRORS";
  else completionStatus = "COMPLETE";
  File result = SD.open(sessionDir + "/test-result.json", FILE_WRITE);
  if (result) {
    const uint64_t sessionDurationUs = closedUs - startedUs;
    const uint64_t adcCaptureDurationUs = adcCaptureEndedUs - adcCaptureStartedUs;
    result.printf("{\"pass\":%s,\"schema_version\":\"knx-long-session-1.1\",\"acquisition_mode\":\"%s\",\"source\":\"ADC_GPIO1\","
                  "\"duration_us\":\"%llu\",\"session_duration_us\":\"%llu\",\"adc_capture_duration_us\":\"%llu\","
                  "\"samples\":\"%llu\",\"chunks\":\"%llu\","
                  "\"sample_rate_measured_hz\":%.6f,\"raw_sha256\":\"%s\","
                  "\"raw_bytes\":\"%llu\",\"stored_samples\":\"%llu\",\"ignored_samples\":\"%llu\","
                  "\"events_total\":\"%llu\",\"excursions\":\"%llu\",\"adc_min\":%u,\"adc_max\":%u,"
                  "\"clipping_low\":\"%llu\",\"clipping_high\":\"%llu\","
                  "\"adc_mean\":%.3f,\"d44_max\":%u,\"dma_overflow\":%u,\"adc_read_errors\":%u,"
                  "\"first_dma_overflow_us\":\"%llu\",\"first_dma_overflow_sample\":\"%llu\","
                  "\"first_pool_exhaustion_us\":\"%llu\",\"first_pool_exhaustion_sample\":\"%llu\","
                  "\"chunk_lifecycle_errors\":%u,\"released_selected_chunks\":%u,"
                  "\"raw_handle_failures\":%u,\"recovered_R1\":%u,"
                  "\"retry_failures\":%u,\"reopen_failures\":%u,\"sd_errors\":%u,\"data_loss\":\"%llu\","
                  "\"pool_exhaustion\":%u,\"free_min\":%u,\"pending_max\":%u,\"ready_max\":%u,"
                  "\"write_us_min\":%u,\"write_us_avg\":%.3f,\"write_us_max\":%u,\"write_max_chunk\":%u,"
                  "\"checkpoint_us_min\":%u,\"checkpoint_us_avg\":%.3f,\"checkpoint_us_max\":%u,"
                  "\"checkpoint_max_chunk\":%u,\"writer_hold_us_max\":%u,\"writer_hold_max_chunk\":%u,"
                  "\"session_crc32\":\"%08X\",\"invariant\":%s,\"closed\":true,"
                  "\"lifecycle\":\"CLOSED\",\"completion_status\":\"%s\",\"resilience_pass\":%s,"
                  "\"fault_injected\":%u,\"real_io_errors\":%u,\"synthetic_r3_count\":%u,"
                  "\"gap_count\":%u,\"gap_sample_start\":\"%llu\",\"gap_sample_end\":\"%llu\","
                  "\"gap_previous_stored_end\":\"%llu\",\"gap_resume_sample_start\":\"%llu\","
                  "\"storage_outage_count\":%u,\"storage_recovery_count\":%u,\"storage_state\":\"%s\","
                  "\"storage_recovery_attempts\":%u,\"storage_recovery_failures\":%u,"
                  "\"storage_recovery_successes\":%u,\"synthetic_recovery_failures\":%u,"
                  "\"current_recovery_backoff_ms\":%u,\"max_recovery_backoff_ms\":%u,"
                  "\"lost_chunks_total\":%u,\"longest_gap_us\":\"%llu\","
                  "\"cumulative_storage_outage_us\":\"%llu\",\"fault_outage_injected\":%u,"
                  "\"fault_recovery_injected\":%u,\"capture_heap_internal_min\":%u,\"capture_heap_dma_min\":%u,"
                  "\"capture_largest_internal_min\":%u,\"capture_largest_dma_min\":%u}\n",
                  finalPass ? "true" : "false", captureModeName(captureMode), sessionDurationUs, sessionDurationUs,
                  adcCaptureDurationUs, producedSamples.load(), producedChunks.load(),
                  adcCaptureDurationUs ? double(storedSamples.load()) * 1000000.0 / double(adcCaptureDurationUs) : 0.0, rawShaHex,
                  rawBytes.load(), storedSamples.load(), ignoredSamples.load(), eventCount, excursionCount,
                  adcMin == UINT16_MAX ? 0 : adcMin, adcMax,
                  adcClipLow, adcClipHigh,
                  producedSamples.load() ? double(adcSum) / producedSamples.load() : 0.0, d44Max,
                  dmaOverflows.load(), adcReadErrors.load(), firstDmaOverflowUs, firstDmaOverflowSample,
                  firstPoolExhaustionUs, firstPoolExhaustionSample, chunkLifecycleErrors.load(), releasedSelectedChunks.load(),
                  rawHandleFailures.load(), r1Count.load(), retryFailures.load(), reopenFailures.load(),
                  sdErrors.load(), lostSamples.load(), poolExhaustion.load(), freeMin, pendingMax, readyMax,
                  writeCount ? writeLatencyMinUs : 0, writeCount ? double(writeLatencyTotalUs) / writeCount : 0,
                  writeLatencyMaxUs, writeLatencyMaxChunk,
                  checkpointCount ? checkpointLatencyMinUs : 0,
                  checkpointCount ? double(checkpointLatencyTotalUs) / checkpointCount : 0,
                  checkpointLatencyMaxUs, checkpointLatencyMaxChunk, writerHoldMaxUs, writerHoldMaxChunk,
                  sessionCrc, invariantOk ? "true" : "false", completionStatus,
                  finalPass ? "true" : "false", faultInjected.load(), rawHandleFailures.load(),
                  syntheticR3Count.load(), gapCount.load(), gapSampleStart, gapSampleEnd,
                  gapPreviousStoredEnd, gapResumeSampleStart, storageOutageCount.load(), storageRecoveryCount.load(),
                  storageStateName(storageState.load()), storageRecoveryAttempts.load(), storageRecoveryFailures.load(),
                  storageRecoverySuccesses.load(), syntheticRecoveryFailures.load(), currentRecoveryBackoffMs,
                  maxRecoveryBackoffMs, lostChunksTotal.load(), longestGapUs, cumulativeStorageOutageUs,
                  faultOutageConsumed ? 1U : 0U, faultRecoveryConsumed ? 1U : 0U,
                  captureInternalMin, captureDmaMin, captureInternalLargestMin, captureDmaLargestMin);
    result.flush(); result.close();
  } else {
    lastSdErrorSource = "finalization"; ++sdErrors; finalPass = false;
    Serial.printf("{\"type\":\"SD_ERROR\",\"source\":\"finalization\",\"count\":%u}\n", sdErrors.load());
  }
  File sessionEnd = SD.open(sessionDir + "/session-end.json", FILE_WRITE);
  if (sessionEnd) {
    sessionEnd.printf("{\"schema_version\":\"knx-long-session-1.1\",\"session_id\":\"%s\",\"acquisition_mode\":\"%s\","
      "\"lifecycle\":\"%s\",\"completion_status\":\"%s\",\"end_monotonic_us\":\"%llu\","
      "\"duration_us\":\"%llu\",\"session_duration_us\":\"%llu\",\"adc_capture_duration_us\":\"%llu\","
      "\"total_samples\":\"%llu\",\"total_raw_bytes\":\"%llu\","
      "\"chunk_count\":\"%llu\",\"gap_count\":%u,\"lost_samples\":\"%llu\",\"raw_sha256\":\"%s\"}\n",
      sessionDir.substring(1).c_str(), captureModeName(captureMode), finalPass ? "CLOSED" : "FAILED", completionStatus, closedUs,
      closedUs - startedUs, closedUs - startedUs, adcCaptureEndedUs - adcCaptureStartedUs,
      producedSamples.load(), rawBytes.load(), storedChunks, gapCount.load(), lostSamples.load(), rawShaHex);
    sessionEnd.close();
  }
  File manifest = SD.open(sessionDir + "/manifest.json", FILE_WRITE);
  if (manifest) {
    const String campaignFields = fieldCampaignJson();
    manifest.printf("{\"schema_version\":\"knx-continuous-raw-manifest-1.0\",\"session_id\":\"%s\","
      "\"acquisition_mode\":\"%s\",\"sample_format\":\"uint16_le\",\"sample_rate_configured_hz\":%u,"
      "\"field_campaign\":%s,"
      "\"session_duration_us\":\"%llu\",\"adc_capture_duration_us\":\"%llu\","
      "\"total_samples\":\"%llu\",\"total_raw_bytes\":\"%llu\",\"chunk_count\":\"%llu\","
      "\"chunk_crc\":\"CRC32_IEEE\",\"logical_raw_sha256\":\"%s\",\"gap_count\":%u,"
      "\"continuity_complete\":%s}\n", sessionDir.substring(1).c_str(), captureModeName(captureMode), SAMPLE_RATE,
      campaignFields.c_str(),
      closedUs - startedUs, adcCaptureEndedUs - adcCaptureStartedUs,
      producedSamples.load(), rawBytes.load(), storedChunks, rawShaHex, gapCount.load(),
      (gapCount.load() == 0 && lostSamples.load() == 0 && invariantOk) ? "true" : "false");
    manifest.close();
  }
  state = finalPass ? State::CLOSED : State::FAILED;
  if (state.load() == State::CLOSED) startIdleObservation();
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
                "\"synthetic_r3_count\":%u,\"gap_count\":%u,\"storage_state\":\"%s\","
                "\"fault_outage_start_chunk\":\"%llu\",\"fault_outage_duration_ms\":%u,"
                "\"fault_outage_consumed\":%s,\"storage_outage_count\":%u,\"storage_recovery_count\":%u,"
                "\"fault_recovery_start_chunk\":\"%llu\",\"fault_recovery_failures_requested\":%u,"
                "\"fault_recovery_consumed\":%s,\"storage_recovery_attempts\":%u,"
                "\"storage_recovery_failures\":%u,\"storage_recovery_successes\":%u,"
                "\"synthetic_recovery_failures\":%u,\"current_recovery_backoff_ms\":%u,"
                "\"max_recovery_backoff_ms\":%u,"
                "\"lost_chunks_total\":%u,\"longest_gap_us\":\"%llu\","
                "\"cumulative_storage_outage_us\":\"%llu\"}\n",
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
                rawHandleFailures.load(), syntheticR3Count.load(), gapCount.load(), storageStateName(storageState.load()),
                faultOutageStartChunk, faultOutageDurationMs, faultOutageConsumed ? "true" : "false",
                storageOutageCount.load(), storageRecoveryCount.load(), faultRecoveryStartChunk,
                faultRecoveryFailuresRequested, faultRecoveryConsumed ? "true" : "false",
                storageRecoveryAttempts.load(), storageRecoveryFailures.load(), storageRecoverySuccesses.load(),
                syntheticRecoveryFailures.load(), currentRecoveryBackoffMs, maxRecoveryBackoffMs,
                lostChunksTotal.load(), longestGapUs,
                cumulativeStorageOutageUs);
  Serial.printf("{\"type\":\"TIME_REFERENCE\",\"session_dir\":\"%s\",\"time_schema_version\":\"1.0\","
                "\"time_source\":\"%s\",\"absolute_time_valid\":%s,\"start_unix_ms\":",
                sessionDir.c_str(), timeSourceName(activeTimeSource), absoluteTimeValid ? "true" : "false");
  if (absoluteTimeValid) Serial.printf("%llu,\"start_utc\":\"%s\",\"browser_timezone_offset_min\":%d,",
                                      startUnixMs, startUtc, activeTimezoneOffsetMin);
  else Serial.print("null,\"start_utc\":null,\"browser_timezone_offset_min\":null,");
  Serial.printf("\"monotonic_origin_us\":\"%llu\"}\n", startedUs);
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

uint64_t jsonStringU64(const String &line, const char *name, uint64_t fallback) {
  const String needle = "\"" + String(name) + "\":\"";
  const int start = line.indexOf(needle);
  if (start < 0) return fallback;
  const int valueStart = start + needle.length();
  const int valueEnd = line.indexOf('"', valueStart);
  return valueEnd > valueStart ? strtoull(line.substring(valueStart, valueEnd).c_str(), nullptr, 10) : fallback;
}

void inspectSession(const String &folder, uint64_t targetSeq) {
  if (state.load() == State::RUNNING || state.load() == State::STOPPING) {
    Serial.println("ERROR INSPECT ACQUISITION_ACTIVE");
    return;
  }
  const String base = "/KNX-" + folder;
  File gapSource = SD.open(base + "/gaps.jsonl", FILE_READ);
  const String gapLine = gapSource ? gapSource.readStringUntil('\n') : String();
  if (gapSource) gapSource.close();
  const uint64_t inspectedGapStart = jsonStringU64(gapLine, "sample_start", (targetSeq - 1) * uint64_t(CHUNK_SAMPLES));
  const uint64_t inspectedGapEnd = jsonStringU64(gapLine, "sample_end", inspectedGapStart + CHUNK_SAMPLES);
  const uint64_t inspectedFirstLost = jsonStringU64(gapLine, "first_lost_chunk_seq", targetSeq);
  const uint64_t inspectedLastLost = jsonStringU64(gapLine, "last_lost_chunk_seq", targetSeq);
  uint32_t actualCrc = 0;
  uint64_t actualBytes = 0;
  for (uint32_t index = 0; index < 2; ++index) {
    File file = SD.open(base + "/" + rawName(index), FILE_READ);
    const uint64_t size = file ? file.size() : 0;
    Serial.printf("{\"type\":\"INSPECT_RAW\",\"segment_index\":%u,\"present\":%s,\"bytes\":\"%llu\"}\n",
                  index, file ? "true" : "false", size);
    uint8_t buffer[512];
    while (file && file.available()) {
      const size_t count = file.read(buffer, sizeof(buffer));
      actualCrc = esp_crc32_le(actualCrc, buffer, count);
      actualBytes += count;
    }
    if (file) file.close();
  }
  File chunksFile = SD.open(base + "/chunks.jsonl", FILE_READ);
  bool beforeFound = false, gapChunkFound = false, afterFound = false;
  while (chunksFile && chunksFile.available()) {
    const String line = chunksFile.readStringUntil('\n');
    const uint64_t seq = jsonStringU64(line, "seq", 0);
    if (seq == inspectedFirstLost - 1) { beforeFound = true; Serial.println(line); }
    if (seq >= inspectedFirstLost && seq <= inspectedLastLost) { gapChunkFound = true; Serial.println(line); }
    if (seq == inspectedLastLost + 1) { afterFound = true; Serial.println(line); }
  }
  if (chunksFile) chunksFile.close();
  Serial.printf("{\"type\":\"INSPECT_SUMMARY\",\"raw_bytes\":\"%llu\",\"crc32\":\"%08X\","
                "\"pattern_check\":\"NOT_APPLICABLE_REAL_ADC\",\"chunk_before_present\":%s,\"chunk_target_present\":%s,"
                "\"chunk_after_present\":%s,\"first_lost_chunk_seq\":\"%llu\","
                "\"last_lost_chunk_seq\":\"%llu\",\"gap_sample_start\":\"%llu\","
                "\"gap_sample_end\":\"%llu\"}\n",
                actualBytes, actualCrc, beforeFound ? "true" : "false",
                gapChunkFound ? "true" : "false", afterFound ? "true" : "false", inspectedFirstLost,
                inspectedLastLost, inspectedGapStart, inspectedGapEnd);
  printFile(base + "/gaps.jsonl", "gaps.jsonl");
  printFile(base + "/storage-transitions.jsonl", "storage-transitions.jsonl");
  printFile(base + "/segments.jsonl", "segments.jsonl");
  printFile(base + "/session-start.json", "session-start.json");
  printFile(base + "/test-result.json", "test-result.json");
}

#ifndef S3_NETWORK_ENABLED
#define S3_NETWORK_ENABLED 1
#endif
#if S3_NETWORK_ENABLED
namespace s3net { void printDiagnostics(); bool configureOtaPassword(const String &password); }
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
  else if (command.startsWith("OTAPASS ")) Serial.println(s3net::configureOtaPassword(command.substring(8)) ? "OK OTAPASS" : "ERROR OTAPASS");
#endif
  else if (command.startsWith("FAULT R3_ONCE ")) {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else {
      const uint64_t target = strtoull(command.substring(14).c_str(), nullptr, 10);
      if (!target) Serial.println("ERROR FAULT CHUNK_SEQ");
      else {
        faultR3OnceChunk = target; faultR3OnceConsumed = false;
        faultOutageStartChunk = 0; faultOutageDurationMs = 0; faultOutageConsumed = false;
        faultRecoveryStartChunk = 0; faultRecoveryFailuresRequested = 0; faultRecoveryConsumed = false;
        Serial.printf("OK FAULT R3_ONCE %llu\n", target);
      }
    }
  } else if (command.startsWith("FAULT RECOVERY_ONCE ")) {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else {
      const int separator = command.indexOf(' ', 20);
      const uint64_t target = separator > 0 ? strtoull(command.substring(20, separator).c_str(), nullptr, 10) : 0;
      const uint32_t failures = separator > 0 ? strtoul(command.substring(separator + 1).c_str(), nullptr, 10) : 0;
      if (!target || !failures) Serial.println("ERROR FAULT RECOVERY_ARGUMENTS");
      else {
        faultRecoveryStartChunk = target; faultRecoveryFailuresRequested = failures; faultRecoveryConsumed = false;
        faultR3OnceChunk = 0; faultR3OnceConsumed = false;
        faultOutageStartChunk = 0; faultOutageDurationMs = 0; faultOutageConsumed = false;
        Serial.printf("OK FAULT RECOVERY_ONCE %llu %u\n", target, failures);
      }
    }
  } else if (command.startsWith("FAULT OUTAGE_ONCE ")) {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else {
      const int separator = command.indexOf(' ', 18);
      const uint64_t target = separator > 0 ? strtoull(command.substring(18, separator).c_str(), nullptr, 10) : 0;
      const uint32_t durationMs = separator > 0 ? strtoul(command.substring(separator + 1).c_str(), nullptr, 10) : 0;
      if (!target || !durationMs) Serial.println("ERROR FAULT OUTAGE_ARGUMENTS");
      else {
        faultOutageStartChunk = target; faultOutageDurationMs = durationMs; faultOutageConsumed = false;
        faultR3OnceChunk = 0; faultR3OnceConsumed = false;
        faultRecoveryStartChunk = 0; faultRecoveryFailuresRequested = 0; faultRecoveryConsumed = false;
        Serial.printf("OK FAULT OUTAGE_ONCE %llu %u\n", target, durationMs);
      }
    }
  } else if (command == "FAULT OFF") {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR FAULT ACQUISITION_ACTIVE");
    else {
      faultR3OnceChunk = 0; faultR3OnceConsumed = false;
      faultOutageStartChunk = 0; faultOutageDurationMs = 0; faultOutageConsumed = false;
      faultRecoveryStartChunk = 0; faultRecoveryFailuresRequested = 0; faultRecoveryConsumed = false;
      Serial.println("OK FAULT OFF");
    }
  } else if (command == "MODE EVENT" || command == "MODE CONTINUOUS" || command == "MODE CONTINUOUS_RAW") {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR MODE ACQUISITION_ACTIVE");
    else { captureMode = command == "MODE EVENT" ? CaptureMode::EVENT : CaptureMode::CONTINUOUS_RAW;
      pendingCaptureMode = captureMode; Serial.printf("OK MODE %s\n", captureModeName(captureMode)); }
  } else if (command.startsWith("THRESHOLD ")) {
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) Serial.println("ERROR THRESHOLD ACQUISITION_ACTIVE");
    else { detection.d44Threshold = strtoul(command.substring(10).c_str(), nullptr, 10);
      Serial.printf("OK THRESHOLD %u\n", detection.d44Threshold); }
  } else if (command.startsWith("START")) {
    if (captureMode == CaptureMode::EVENT && !calibration.valid()) {
      Serial.printf("ERROR START CALIBRATION_REQUIRED %s %s\n",
                    autocal::stateName(calibration.state()), calibration.reason());
      return;
    }
    if (command.length() > 5) {
      const uint32_t seconds = strtoul(command.substring(6).c_str(), nullptr, 10);
      if (!seconds) { Serial.println("ERROR START DURATION"); return; }
      requestedDurationUs = uint64_t(seconds) * 1000000ULL;
    }
#if S3_NETWORK_ENABLED
    prepareTimeNone();
    stopIdleObservation();
    if (!s3net::stopForCapture()) { startIdleObservation(); Serial.println("ERROR START WIFI_NOT_OFF"); return; }
#endif
    if (startRun()) Serial.println("OK START");
    else { startIdleObservation(); Serial.println("ERROR START"); }
  }
  else if (command == "STOP") {
    if (state.load() != State::RUNNING) Serial.println("ERROR STOP");
    else { finalizeRun(); Serial.println("OK STOP"); }
  } else if (command.length()) Serial.println("ERROR COMMAND");
}

#if S3_NETWORK_ENABLED
#include "network_services.h"
#endif

void setup() {
  Serial.begin(115200);
  delay(1200);
  const size_t psramBytes = ESP.getPsramSize();
  const bool psramReady = psramFound() && psramBytes >= board::REQUIRED_PSRAM_BYTES;
  Serial.printf("{\"type\":\"PSRAM_CHECK\",\"required\":true,\"detected\":%s,\"bytes\":%u,\"minimum_bytes\":%u}\n",
                psramReady ? "true" : "false", unsigned(psramBytes), unsigned(board::REQUIRED_PSRAM_BYTES));
  if (!psramReady) {
    fatalConfiguration = true;
    state = State::FAILED;
    Serial.println("{\"type\":\"FATAL_CONFIGURATION\",\"reason\":\"PSRAM_REQUIRED\",\"action\":\"Build with PSRAM=opi\"}");
    return;
  }
  sdSpi.begin(board::SD_SCK, board::SD_MISO, board::SD_MOSI, board::SD_CS);
  sdReady = SD.begin(board::SD_CS, sdSpi, SD_HZ, "/sd", SD_MAX_FILES);
  readyQ = xQueueCreate(CHUNK_COUNT, sizeof(Chunk *));
  bool poolReady = readyQ;
  for (uint32_t i = 0; i < CHUNK_COUNT && poolReady; ++i) {
    chunks[i] = static_cast<Chunk *>(heap_caps_calloc(1, sizeof(Chunk), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    poolReady = chunks[i] != nullptr;
  }
  const bool adcReady = initAdc();
  adcDmaBuffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
      4, DMA_FRAME_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
  const bool adcBufferReady = adcDmaBuffer != nullptr;
  if (poolReady) {
    if (adcReady && adcBufferReady)
      xTaskCreatePinnedToCore(producerTask, "adc", ADC_TASK_STACK_BYTES, nullptr, 4, &producerHandle, 1);
    xTaskCreatePinnedToCore(writerTask, "writer", 4096, nullptr, S3_WRITER_PRIORITY, &writerHandle, S3_WRITER_CORE);
  }
  delay(100);
  preWifiHeapOk = heap_caps_check_integrity_all(true);
  preWifiInternalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  preWifiInternalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  preWifiInternalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  preWifiDmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  preWifiDmaMin = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  preWifiDmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  Serial.printf("{\"type\":\"PRE_WIFI_MEMORY\",\"adc_task_created\":%s,\"heap_ok\":%s,"
                "\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,"
                "\"dma_free\":%u,\"dma_min\":%u,\"dma_largest\":%u}\n",
                producerHandle ? "true" : "false",
                preWifiHeapOk ? "true" : "false", preWifiInternalFree, preWifiInternalMin,
                preWifiInternalLargest, preWifiDmaFree, preWifiDmaMin, preWifiDmaLargest);
  Serial.printf("{\"type\":\"BOOT\",\"firmware\":\"KNXAnalyzerField-s3-analog-v0\",\"board\":\"%s\",\"sd\":%s,"
                "\"card_type\":%u,\"sd_max_files\":%u,\"pool\":%s,\"adc\":%s,\"adc_gpio\":1,"
                "\"adc_dma_buffer\":%s,\"adc_task_stack_bytes\":%u,"
                "\"future_uart_tx\":43,\"future_uart_rx\":44,\"camera\":false,\"microphone\":false,"
                "\"sample_rate_target_hz\":%u}\n", board::NAME,
                sdReady ? "true" : "false", unsigned(SD.cardType()), unsigned(SD_MAX_FILES),
                poolReady ? "true" : "false", adcReady ? "true" : "false",
                adcBufferReady ? "true" : "false", ADC_TASK_STACK_BYTES, SAMPLE_RATE);
  if (!sdReady || !poolReady || !adcReady || !adcBufferReady || !producerHandle) state = State::FAILED;
  calibration.begin();
  if (state.load() != State::FAILED && !startIdleObservation()) state = State::FAILED;
#if S3_NETWORK_ENABLED
  s3net::begin();
#endif
}

void loop() {
  if (fatalConfiguration) {
    delay(1000);
    return;
  }
  calibration.servicePersistence();
#if S3_NETWORK_ENABLED
  s3net::tick();
#endif
  if (state.load() == State::RUNNING && captureMode == CaptureMode::CONTINUOUS_RAW) {
    const uint64_t elapsedUs = esp_timer_get_time() - startedUs;
    if (elapsedUs >= lastRawDebugUs + 10000000ULL) {
      lastRawDebugUs = elapsedUs;
      Serial.printf("RAW t=%llus samples=%llu stored=%llu bytes=%llu free=%u ready=%u loss=%llu gaps=%u dma=%u adc=%u sd=%u\n",
                    elapsedUs / 1000000ULL, producedSamples.load(), storedSamples.load(), rawBytes.load(),
                    freeMin, readyQ ? unsigned(uxQueueMessagesWaiting(readyQ)) : 0U,
                    lostSamples.load(), gapCount.load(), dmaOverflows.load(), adcReadErrors.load(), sdErrors.load());
    }
  }
  if (state.load() == State::RUNNING && esp_timer_get_time() - startedUs >= requestedDurationUs) finalizeRun();
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
