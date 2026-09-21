#include "storage.h"

#include <SD.h>
#include <SPI.h>
#include <esp_system.h>

namespace storage {
namespace {
constexpr uint32_t kChunkSamples = 512;
constexpr char kMagic[8] = {'K','N','X','A','D','C','1','\0'};
bool mounted = false;
bool detected = false;
uint8_t cardType = 0;
uint64_t cardBytes = 0;
uint64_t totalBytes = 0;
uint64_t freeBytes = 0;
uint64_t bytesWritten = 0;
uint32_t writeErrors = 0;
uint32_t capturesPersisted = 0;
uint32_t capturesFailed = 0;
uint8_t queueHighWater = 0;
char activeId[24] = {};
char lastId[24] = {};
uint32_t sessionStartMs = 0;
uint32_t sessionStartOverruns = 0;
uint32_t sessionStartReadErrors = 0;
uint32_t sessionStartHttpErrors = 0;
uint32_t sessionEventCount = 0;
uint32_t lastSessionEventCount = 0;
uint32_t sessionPersisted = 0;
uint32_t sessionFailed = 0;
uint32_t sessionLastRate = 0;
uint32_t sessionHttpErrors = 0;
uint32_t lastCapacityMs = 0;
events::Event pendingEvent = {};
bool pending = false;
File captureFile;
uint32_t writeOffset = 0;
uint32_t queuedAtMs = 0;
uint32_t maxChunkUs = 0;
uint32_t crc = 0xFFFFFFFFUL;
uint16_t sampleChunk[kChunkSamples];
String tempPath;
String finalPath;

String sessionDir(const String &id) { return String(kSessions) + "/" + id; }
String sessionPath(const String &id) { return sessionDir(id) + "/session.json"; }
String eventsPath(const String &id) { return sessionDir(id) + "/events.jsonl"; }
String capturePath(const String &id, uint32_t eventId, bool temporary = false) {
  char name[36];
  snprintf(name, sizeof(name), "/event-%06lu.bin%s", static_cast<unsigned long>(eventId), temporary ? ".part" : "");
  return sessionDir(id) + "/captures" + name;
}
void error(const char *stage) {
  ++writeErrors;
  Serial.printf("{\"type\":\"SD_WRITE_ERROR\",\"stage\":\"%s\",\"count\":%lu}\n", stage, static_cast<unsigned long>(writeErrors));
}
void refreshCapacity() {
  if (!mounted) return;
  totalBytes = SD.totalBytes();
  const uint64_t used = SD.usedBytes();
  freeBytes = totalBytes >= used ? totalBytes - used : 0;
  lastCapacityMs = millis();
}
uint32_t crcStep(uint32_t value, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    value ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) value = (value >> 1) ^ ((value & 1) ? 0xEDB88320UL : 0);
  }
  return value;
}
bool writeDocumentAtomic(const String &path, JsonDocument &doc) {
  const String temp = path + ".tmp";
  const String backup = path + ".bak";
  if (SD.exists(temp)) SD.remove(temp); // only a project-owned temporary file
  File file = SD.open(temp, FILE_WRITE);
  if (!file) { error("open_metadata_temp"); return false; }
  const size_t written = serializeJson(doc, file);
  file.println();
  file.flush();
  file.close();
  if (!written) { SD.remove(temp); error("write_metadata_temp"); return false; }
  bytesWritten += written + 1;
  if (SD.exists(backup)) SD.remove(backup);
  if (SD.exists(path) && !SD.rename(path, backup)) {
    SD.remove(temp); error("backup_metadata"); return false;
  }
  if (!SD.rename(temp, path)) {
    if (SD.exists(backup)) SD.rename(backup, path);
    error("commit_metadata"); return false;
  }
  if (SD.exists(backup)) SD.remove(backup);
  return true;
}
uint32_t countLines(const String &path) {
  File file = SD.open(path, FILE_READ);
  if (!file) return 0;
  uint32_t count = 0;
  while (file.available()) if (file.read() == '\n') ++count;
  file.close();
  return count;
}
bool writeActiveSession(const char *state, bool closed, uint32_t endMs) {
  if (!mounted || !activeId[0]) return false;
  const auto adc = scope::status();
  JsonDocument doc;
  doc["session_id"] = activeId;
  doc["state"] = state;
  doc["start_uptime_ms"] = sessionStartMs;
  if (closed) {
    doc["end_uptime_ms"] = endMs;
    doc["duration_ms"] = endMs - sessionStartMs;
  } else {
    doc["end_uptime_ms"] = nullptr;
    doc["duration_ms"] = nullptr;
  }
  doc["date_time"] = nullptr; // no NTP dependency
  doc["event_count"] = sessionEventCount;
  doc["adc_sample_rate_hz"] = sessionLastRate ? sessionLastRate : adc.requestedHz;
  doc["adc_overruns"] = adc.overruns - sessionStartOverruns;
  doc["dma_errors"] = adc.readErrors - sessionStartReadErrors;
  doc["http_errors"] = sessionHttpErrors - sessionStartHttpErrors;
  doc["http_timeouts"] = nullptr;
  doc["captures_persisted"] = sessionPersisted;
  doc["captures_failed"] = sessionFailed;
  doc["sd_write_errors"] = writeErrors;
  doc["firmware"] = "0.5.0-sessions";
  doc["hardware"] = "Waveshare ESP32-C6-Touch-LCD-1.47";
  doc["capture_format"] = kFormatVersion;
  return writeDocumentAtomic(sessionPath(activeId), doc);
}
bool appendEvent(const events::Event &event, bool persisted, uint32_t sampleCrc, const char *failure) {
  const String path = eventsPath(event.sessionId);
  File file = SD.open(path, FILE_APPEND);
  if (!file) { error("open_events_jsonl"); return false; }
  JsonDocument doc;
  doc["event_id"] = event.eventId;
  doc["session_id"] = event.sessionId;
  doc["uptime_ms"] = event.uptimeMs;
  doc["trigger"] = events::triggerName(event.triggerType);
  doc["sample_rate_hz"] = event.sampleRate;
  doc["sample_count"] = event.sampleCount;
  doc["trigger_index"] = event.triggerIndex;
  doc["pre_trigger_samples"] = event.preTriggerSamples;
  doc["post_trigger_samples"] = event.postTriggerSamples;
  doc["adc_min"] = event.adcMin;
  doc["adc_max"] = event.adcMax;
  doc["adc_mean_before"] = event.adcMeanBefore;
  doc["adc_mean_after"] = event.adcMeanAfter;
  doc["raw_persisted"] = persisted;
  doc["raw_bytes"] = persisted ? sizeof(CaptureHeader) + event.sampleCount * sizeof(uint16_t) : 0;
  doc["raw_crc32"] = persisted ? sampleCrc : 0;
  if (failure) doc["error"] = failure;
  const size_t written = serializeJson(doc, file);
  file.println();
  file.flush();
  file.close();
  if (!written) { error("write_events_jsonl"); return false; }
  bytesWritten += written + 1;
  return true;
}
void finishJob(bool okay, const char *failure) {
  if (captureFile) captureFile.close();
  if (!okay && mounted && tempPath.length() && SD.exists(tempPath)) SD.remove(tempPath);
  if (okay) {
    ++capturesPersisted;
    ++sessionPersisted;
    events::markPersisted(pendingEvent.eventId);
  } else {
    ++capturesFailed;
    ++sessionFailed;
    error(failure ? failure : "capture");
  }
  const uint32_t finalCrc = crc ^ 0xFFFFFFFFUL;
  appendEvent(pendingEvent, okay, finalCrc, okay ? nullptr : failure);
  writeActiveSession("RUNNING", false, 0);
  refreshCapacity();
  Serial.printf("{\"type\":\"SD_CAPTURE\",\"event_id\":%lu,\"persisted\":%s,\"bytes\":%lu,\"duration_ms\":%lu,\"max_chunk_us\":%lu,\"queue_depth\":0}\n",
                static_cast<unsigned long>(pendingEvent.eventId), okay ? "true" : "false",
                static_cast<unsigned long>(okay ? sizeof(CaptureHeader) + pendingEvent.sampleCount * 2 : 0),
                static_cast<unsigned long>(millis() - queuedAtMs), static_cast<unsigned long>(maxChunkUs));
  pending = false;
  tempPath = "";
  finalPath = "";
}
void recoverSessions() {
  if (!SD.exists(kSessions)) return;
  File root = SD.open(kSessions);
  if (!root || !root.isDirectory()) return;
  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      String id = entry.name();
      const int slash = id.lastIndexOf('/');
      if (slash >= 0) id = id.substring(slash + 1);
      if (validSessionId(id)) {
        const String path = sessionPath(id);
        const String backup = path + ".bak";
        if (!SD.exists(path) && SD.exists(backup)) SD.rename(backup, path);
        JsonDocument doc;
        if (readSession(id, doc) && String(doc["state"] | "") == "RUNNING") {
          doc["state"] = "INTERRUPTED";
          doc["end_uptime_ms"] = nullptr;
          doc["duration_ms"] = nullptr;
          doc["event_count"] = countLines(eventsPath(id));
          writeDocumentAtomic(path, doc);
          Serial.printf("{\"type\":\"SESSION_RECOVERED\",\"session_id\":\"%s\",\"state\":\"INTERRUPTED\"}\n", id.c_str());
        }
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
}
bool readHeader(File &file, CaptureHeader &header) {
  if (!file || file.size() < sizeof(header) || file.read(reinterpret_cast<uint8_t *>(&header), sizeof(header)) != sizeof(header)) return false;
  if (memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != kFormatVersion ||
      header.headerBytes != sizeof(header) || header.sampleCount != scope::kRingSamples ||
      header.triggerIndex != scope::kPreSamples || header.preSamples != scope::kPreSamples ||
      header.postSamples != scope::kPostSamples || file.size() != sizeof(header) + header.sampleCount * 2) return false;
  return true;
}
} // namespace

bool validSessionId(const String &id) {
  if (id.length() < 3 || id.length() >= sizeof(activeId)) return false;
  for (size_t i = 0; i < id.length(); ++i) {
    const char c = id[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
  }
  return true;
}

bool begin() {
  mounted = SD.begin(4, SPI, 4000000);
  cardType = mounted ? static_cast<uint8_t>(SD.cardType()) : 0;
  detected = mounted && cardType != CARD_NONE;
  mounted = detected;
  cardBytes = mounted ? SD.cardSize() : 0;
  if (!mounted) { Serial.println("{\"type\":\"SD_STORAGE\",\"mounted\":false}"); return false; }
  refreshCapacity();
  recoverSessions();
  Serial.printf("{\"type\":\"SD_STORAGE\",\"mounted\":true,\"total_bytes\":%llu,\"free_bytes\":%llu}\n", totalBytes, freeBytes);
  return true;
}
Status status() {
  return {detected, mounted, cardType, cardBytes, totalBytes, freeBytes, bytesWritten, writeErrors,
          capturesPersisted, capturesFailed, sessionEventCount, lastSessionEventCount,
          static_cast<uint8_t>(pending ? 1 : 0), queueHighWater,
          activeId, lastId};
}
const char *currentSessionId() { return activeId; }
const char *lastSessionId() { return lastId; }
bool busy() { return pending; }

bool startSession(uint32_t httpErrors) {
  if (!mounted || activeId[0] || pending) return false;
  if (!SD.exists(kRoot) && !SD.mkdir(kRoot)) { error("mkdir_root"); return false; }
  if (!SD.exists(kSessions) && !SD.mkdir(kSessions)) { error("mkdir_sessions"); return false; }
  const uint32_t chip = static_cast<uint32_t>(ESP.getEfuseMac());
  bool unique = false;
  for (uint8_t attempt = 0; attempt < 8; ++attempt) {
    snprintf(activeId, sizeof(activeId), "s-%06lx-%08lx", static_cast<unsigned long>(chip & 0xFFFFFFUL), static_cast<unsigned long>(esp_random()));
    if (!SD.exists(sessionDir(activeId))) { unique = true; break; }
  }
  if (!unique) { activeId[0] = 0; error("session_id_collision"); return false; }
  if (!SD.mkdir(sessionDir(activeId)) || !SD.mkdir(sessionDir(activeId) + "/captures")) {
    error("mkdir_session"); activeId[0] = 0; return false;
  }
  sessionStartMs = millis();
  const auto adc = scope::status();
  sessionStartOverruns = adc.overruns;
  sessionStartReadErrors = adc.readErrors;
  sessionStartHttpErrors = httpErrors;
  sessionHttpErrors = httpErrors;
  sessionEventCount = 0;
  sessionPersisted = 0;
  sessionFailed = 0;
  sessionLastRate = 0;
  if (!writeActiveSession("RUNNING", false, 0)) { activeId[0] = 0; return false; }
  Serial.printf("{\"type\":\"SESSION_START\",\"session_id\":\"%s\"}\n", activeId);
  return true;
}
bool stopSession(uint32_t httpErrors) {
  if (!activeId[0]) return true;
  flush();
  sessionHttpErrors = httpErrors;
  const bool okay = writeActiveSession("CLOSED", true, millis());
  strlcpy(lastId, activeId, sizeof(lastId));
  lastSessionEventCount = sessionEventCount;
  activeId[0] = 0;
  refreshCapacity();
  Serial.printf("{\"type\":\"SESSION_STOP\",\"session_id\":\"%s\",\"closed\":%s}\n", lastId, okay ? "true" : "false");
  return okay;
}
bool queueEvent(const events::Event &event) {
  if (!mounted || !activeId[0] || pending || strcmp(event.sessionId, activeId) != 0) {
    ++capturesFailed; ++sessionFailed; error("queue_event"); return false;
  }
  pendingEvent = event;
  pending = true;
  if (queueHighWater < 1) queueHighWater = 1;
  ++sessionEventCount;
  sessionLastRate = event.sampleRate;
  writeOffset = 0;
  queuedAtMs = millis();
  maxChunkUs = 0;
  crc = 0xFFFFFFFFUL;
  return true;
}
void tick() {
  if (!mounted) return;
  if (!pending) return;
  if (!captureFile) {
    tempPath = capturePath(pendingEvent.sessionId, pendingEvent.eventId, true);
    finalPath = capturePath(pendingEvent.sessionId, pendingEvent.eventId);
    if (SD.exists(tempPath) || SD.exists(finalPath)) { finishJob(false, "capture_collision"); return; }
    captureFile = SD.open(tempPath, FILE_WRITE);
    if (!captureFile) { finishJob(false, "open_capture"); return; }
    CaptureHeader header = {};
    memcpy(header.magic, kMagic, sizeof(kMagic));
    header.version = kFormatVersion;
    header.headerBytes = sizeof(header);
    header.eventId = pendingEvent.eventId;
    header.sampleRateHz = pendingEvent.sampleRate;
    header.sampleCount = pendingEvent.sampleCount;
    header.triggerIndex = pendingEvent.triggerIndex;
    header.preSamples = pendingEvent.preTriggerSamples;
    header.postSamples = pendingEvent.postTriggerSamples;
    header.adcMin = pendingEvent.adcMin;
    header.adcMax = pendingEvent.adcMax;
    if (captureFile.write(reinterpret_cast<const uint8_t *>(&header), sizeof(header)) != sizeof(header)) {
      finishJob(false, "write_header"); return;
    }
    bytesWritten += sizeof(header);
  }
  while (writeOffset < pendingEvent.sampleCount) {
    const uint32_t count = min(kChunkSamples, pendingEvent.sampleCount - writeOffset);
    if (!scope::copyCaptureSamples(pendingEvent.captureNumber, writeOffset, sampleChunk, count)) {
      finishJob(false, "raw_not_retained"); return;
    }
    const size_t bytes = count * sizeof(uint16_t);
    const uint32_t chunkStart = micros();
    if (captureFile.write(reinterpret_cast<const uint8_t *>(sampleChunk), bytes) != bytes) {
      finishJob(false, "write_samples"); return;
    }
    const uint32_t chunkUs = micros() - chunkStart;
    if (chunkUs > maxChunkUs) maxChunkUs = chunkUs;
    crc = crcStep(crc, reinterpret_cast<const uint8_t *>(sampleChunk), bytes);
    bytesWritten += bytes;
    writeOffset += count;
  }
  const uint32_t finalCrc = crc ^ 0xFFFFFFFFUL;
  if (!captureFile.seek(offsetof(CaptureHeader, samplesCrc32)) ||
      captureFile.write(reinterpret_cast<const uint8_t *>(&finalCrc), sizeof(finalCrc)) != sizeof(finalCrc)) {
    finishJob(false, "write_crc"); return;
  }
  captureFile.flush();
  captureFile.close();
  if (!SD.rename(tempPath, finalPath)) { finishJob(false, "commit_capture"); return; }
  finishJob(true, nullptr);
}
void flush() {
  uint32_t rounds = 0;
  while (pending && rounds++ < 110) tick();
  if (pending) finishJob(false, "flush_timeout");
}
bool readSession(const String &id, JsonDocument &doc) {
  if (!mounted || !validSessionId(id)) return false;
  File file = SD.open(sessionPath(id), FILE_READ);
  if (!file) return false;
  const DeserializationError result = deserializeJson(doc, file);
  file.close();
  return !result;
}
bool listSessions(JsonDocument &doc) {
  JsonArray items = doc["sessions"].to<JsonArray>();
  if (!mounted) return false;
  if (!SD.exists(kSessions)) { doc["count"] = 0; return true; }
  File root = SD.open(kSessions);
  if (!root || !root.isDirectory()) return false;
  File entry = root.openNextFile();
  while (entry) {
    if (entry.isDirectory()) {
      String id = entry.name();
      const int slash = id.lastIndexOf('/');
      if (slash >= 0) id = id.substring(slash + 1);
      JsonDocument metadata;
      if (validSessionId(id) && readSession(id, metadata)) {
        JsonObject item = items.add<JsonObject>();
        item.set(metadata.as<JsonObject>());
      }
    }
    entry.close();
    entry = root.openNextFile();
  }
  root.close();
  doc["count"] = items.size();
  return true;
}
bool listEvents(const String &id, JsonDocument &doc) {
  JsonArray items = doc["events"].to<JsonArray>();
  if (!mounted || !validSessionId(id) || !SD.exists(sessionPath(id))) return false;
  File file = SD.open(eventsPath(id), FILE_READ);
  if (!file) { doc["count"] = 0; return true; }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    JsonDocument event;
    if (!deserializeJson(event, line)) {
      JsonObject item = items.add<JsonObject>();
      item.set(event.as<JsonObject>());
    }
  }
  file.close();
  doc["count"] = items.size();
  return true;
}
bool readEvent(const String &id, uint32_t eventId, JsonDocument &doc) {
  if (!mounted || !validSessionId(id) || eventId == 0) return false;
  File file = SD.open(eventsPath(id), FILE_READ);
  if (!file) return false;
  bool found = false;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    JsonDocument candidate;
    if (!deserializeJson(candidate, line) && candidate["event_id"].as<uint32_t>() == eventId) {
      doc.set(candidate.as<JsonObject>());
      found = true;
      break;
    }
  }
  file.close();
  return found;
}
bool hasCapture(const String &id, uint32_t eventId) {
  return mounted && validSessionId(id) && eventId && SD.exists(capturePath(id, eventId));
}
File openRaw(const String &id, uint32_t eventId) {
  if (!hasCapture(id, eventId)) return File();
  File file = SD.open(capturePath(id, eventId), FILE_READ);
  CaptureHeader header;
  if (!readHeader(file, header) || header.eventId != eventId) { file.close(); return File(); }
  file.seek(0);
  return file;
}
bool readWaveform(const String &id, uint32_t eventId, scope::Waveform &wave, uint32_t &sampleRateHz) {
  File file = openRaw(id, eventId);
  if (!file) return false;
  CaptureHeader header;
  if (!readHeader(file, header)) { file.close(); return false; }
  wave = {};
  wave.captured = true;
  wave.samples = header.sampleCount;
  wave.triggerPosition = header.triggerIndex;
  wave.minRaw = UINT16_MAX;
  for (uint16_t &value : wave.low) value = UINT16_MAX;
  sampleRateHz = header.sampleRateHz;
  uint32_t check = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < header.sampleCount; i += kChunkSamples) {
    const uint32_t count = min(kChunkSamples, header.sampleCount - i);
    const size_t bytes = count * sizeof(uint16_t);
    if (file.read(reinterpret_cast<uint8_t *>(sampleChunk), bytes) != bytes) { file.close(); return false; }
    check = crcStep(check, reinterpret_cast<const uint8_t *>(sampleChunk), bytes);
    for (uint32_t j = 0; j < count; ++j) {
      const uint16_t raw = sampleChunk[j];
      const uint32_t column = ((i + j) * scope::kWaveColumns) / header.sampleCount;
      if (raw < wave.low[column]) wave.low[column] = raw;
      if (raw > wave.high[column]) wave.high[column] = raw;
      if (raw < wave.minRaw) wave.minRaw = raw;
      if (raw > wave.maxRaw) wave.maxRaw = raw;
    }
  }
  file.close();
  return (check ^ 0xFFFFFFFFUL) == header.samplesCrc32;
}
} // namespace storage
