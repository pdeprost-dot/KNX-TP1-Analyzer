#pragma once

// Included inside namespace s3net. This API is strictly read-only.
namespace sessionapi {

constexpr const char *API_VERSION = "1.0";
static uint8_t transferBuffer[4096];
uint64_t transferBytes = 0;
uint32_t transferCount = 0, transferErrors = 0;
uint64_t writeRequestedBytes = 0, writeAcceptedBytes = 0;
uint32_t shortWrites = 0, zeroWrites = 0, writeTimeouts = 0;

bool writeAll(WiFiClient &client, const uint8_t *data, size_t length) {
  size_t sent = 0; uint32_t noProgressSince = millis();
  while (sent < length) {
    const size_t requested = min<size_t>(1360, length - sent);
    writeRequestedBytes += requested;
    const size_t written = client.write(data + sent, requested);
    writeAcceptedBytes += written;
    if (written < requested) ++shortWrites;
    if (written) {
      sent += written; noProgressSince = millis();
      // Let the Wi-Fi/TCP task drain its bounded TX queue before the next slice.
      delay(1);
    } else {
      ++zeroWrites;
      if (millis() - noProgressSince > 5000) { ++writeTimeouts; return false; }
      delay(1);
    }
  }
  return sent == length;
}

bool sendJson(int code, const String &json) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  server.setContentLength(json.length());
  server.send(code, "application/json", "");
  WiFiClient client = server.client();
  client.setNoDelay(true);
  const bool complete = writeAll(client, reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
  if (!complete) ++transferErrors;
  transferBytes += complete ? json.length() : 0;
  ++transferCount;
  client.stop();
  return complete;
}

String quote(const String &value) {
  String out; out.reserve(value.length() + 2); out += '"';
  for (char c : value) {
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (uint8_t(c) >= 0x20) out += c;
  }
  out += '"'; return out;
}

void error(int code, const char *message) {
  sendJson(code, "{\"error\":" + quote(message) + "}");
}

bool safeLeaf(const String &value) {
  if (value.isEmpty() || value == "." || value == ".." || value.indexOf('/') >= 0 ||
      value.indexOf('\\') >= 0 || value.indexOf("..") >= 0) return false;
  for (char c : value)
    if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
  return true;
}

bool safeSession(const String &value) {
  return safeLeaf(value) && (value.startsWith("KNX-") || value.startsWith("EVENT-"));
}

bool allowedFile(const String &name) {
  static constexpr const char *fixed[] = {
    "session-start.json", "test-result.json", "events.jsonl", "chunks.jsonl",
    "segments.jsonl", "chunk-index.jsonl", "checkpoints.jsonl", "sd-incidents.jsonl",
    "gaps.jsonl", "storage-transitions.jsonl", "session-end.json", "manifest.json"
  };
  for (const char *candidate : fixed) if (name == candidate) return true;
  if (!name.startsWith("raw-") || !name.endsWith(".bin") || name.length() != 12) return false;
  for (uint8_t i = 4; i < 8; ++i) if (!isdigit(static_cast<unsigned char>(name[i]))) return false;
  return true;
}

String smallFile(const String &path, size_t limit = 24576) {
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > limit) { if (file) file.close(); return {}; }
  String text = file.readString();
  file.close(); text.trim(); return text;
}

String scalar(const String &json, const char *key) {
  const String needle = quote(key) + ":";
  int pos = json.indexOf(needle); if (pos < 0) return {};
  pos += needle.length(); while (pos < int(json.length()) && isspace(static_cast<unsigned char>(json[pos]))) ++pos;
  if (pos >= int(json.length())) return {};
  if (json[pos] == '"') {
    const int end = json.indexOf('"', pos + 1);
    return end > pos ? json.substring(pos + 1, end) : String();
  }
  int end = pos;
  while (end < int(json.length()) && json[end] != ',' && json[end] != '}') ++end;
  String value = json.substring(pos, end); value.trim(); return value == "null" ? String() : value;
}

uint64_t directoryBytes(const String &base) {
  uint64_t bytes = 0; File directory = SD.open(base);
  if (!directory || !directory.isDirectory()) { if (directory) directory.close(); return 0; }
  for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
    if (!file.isDirectory()) bytes += file.size();
    file.close();
  }
  directory.close(); return bytes;
}

bool available() {
  const State current = state.load();
  return sdReady && current != State::RUNNING && current != State::STOPPING;
}

bool pauseObservation() {
  const bool wasObserving = adcMode.load() == AdcMode::OBSERVING;
  if (wasObserving) stopIdleObservation();
  return wasObserving;
}

void resumeObservation(bool paused) {
  if (paused) startIdleObservation();
}

void analyzer() {
  String id = "S3-"; id += String(uint32_t(ESP.getEfuseMac()), HEX); id.toUpperCase();
  String json = "{\"api_version\":\"1.0\",\"analyzer_id\":" + quote(id) +
    ",\"hostname\":" + quote(hostname) + ",\"firmware_version\":" + quote(FIRMWARE_VERSION) +
    ",\"logger_schema\":\"knx-long-session-1.1\",\"raw_format\":\"segmented-v1\",\"state\":" +
    quote(stateName(state.load())) + ",\"ip\":" + quote(WiFi.localIP().toString()) +
    ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) +
    ",\"sd_ready\":" + String(sdReady ? "true" : "false") +
    ",\"ota_available\":" + String((state.load() == State::IDLE || state.load() == State::CLOSED) ? "true" : "false") +
    ",\"http_write_requested_bytes\":\"" + String(writeRequestedBytes) + "\"" +
    ",\"http_write_accepted_bytes\":\"" + String(writeAcceptedBytes) + "\"" +
    ",\"http_short_writes\":" + String(shortWrites) +
    ",\"http_zero_writes\":" + String(zeroWrites) +
    ",\"http_write_timeouts\":" + String(writeTimeouts) + "}";
  sendJson(200, json);
}

void sessions() {
  if (!available()) { error(409, "capture_active"); return; }
  const bool paused = pauseObservation();
  String json; json.reserve(4096);
  json = "{\"api_version\":\"1.0\",\"sessions\":[";
  bool first = true; File root = SD.open("/");
  if (root) {
    for (File directory = root.openNextFile(); directory; directory = root.openNextFile()) {
      String folder = directory.name(); if (folder.startsWith("/")) folder.remove(0, 1);
      if (directory.isDirectory() && safeSession(folder)) {
        const String base = "/" + folder;
        const String start = smallFile(base + "/session-start.json");
        const String result = smallFile(base + "/test-result.json");
        const String sessionId = scalar(start, "session_id").length() ? scalar(start, "session_id") : folder;
        const String lifecycle = scalar(result, "lifecycle");
        const String stateValue = result.length() ? (lifecycle.length() ? lifecycle : "CLOSED") :
          (start.length() ? "INTERRUPTED" : "UNKNOWN");
        if (!first) json += ','; first = false;
        json += "{\"folder\":" + quote(folder) + ",\"session_id\":" + quote(sessionId) +
          ",\"acquisition_mode\":" + quote(scalar(start, "acquisition_mode").length() ? scalar(start, "acquisition_mode") : scalar(start, "capture_mode")) +
          ",\"state\":" + quote(stateValue) + ",\"completion_status\":" + quote(scalar(result, "completion_status")) +
          ",\"schema_version\":" + quote(scalar(start, "schema_version")) +
          ",\"events\":" + (scalar(result, "events_total").length() ? scalar(result, "events_total") : "null") +
          ",\"valid_raw_bytes\":" + quote(scalar(result, "raw_bytes")) +
          ",\"duration_us\":" + quote(scalar(result, "duration_us")) +
          ",\"start_utc\":" + (scalar(start, "start_utc").length() ? quote(scalar(start, "start_utc")) : "null") +
          ",\"time_source\":" + quote(scalar(start, "time_source")) +
          ",\"calibration_crc\":" + quote(scalar(start, "calibration_id_crc32")) +
          ",\"threshold\":" + (scalar(start, "d44_threshold").length() ? scalar(start, "d44_threshold") : "null") +
          ",\"confidence\":" + quote(scalar(start, "calibration_confidence_category")) +
          ",\"has_session_start\":" + String(start.length() ? "true" : "false") +
          ",\"has_test_result\":" + String(result.length() ? "true" : "false") +
          ",\"useful_bytes_approx\":" + quote(scalar(result, "raw_bytes")) + "}";
      }
      directory.close();
    }
    root.close();
  }
  json += "]}";
  sendJson(200, json);
  resumeObservation(paused);
}

String fileRole(const String &name) {
  if (name == "session-start.json") return "session_start";
  if (name == "test-result.json") return "result";
  if (name == "session-end.json") return "session_end";
  if (name == "manifest.json") return "integrity_manifest";
  if (name == "events.jsonl") return "events";
  if (name == "chunks.jsonl") return "raw_map";
  if (name == "segments.jsonl") return "segments";
  if (name == "chunk-index.jsonl") return "raw_index";
  if (name.startsWith("raw-")) return "raw";
  if (name == "gaps.jsonl") return "gaps";
  if (name == "sd-incidents.jsonl") return "incidents";
  if (name == "storage-transitions.jsonl") return "storage_transitions";
  return "diagnostic";
}

String fileDescriptor(const String &base, const String &session, const String &name, const String &prefix) {
  File file = SD.open(base + "/" + name, FILE_READ);
  if (!file || file.isDirectory()) { if (file) file.close(); return "{\"present\":false}"; }
  const uint64_t size = file.size(); file.close();
  return "{\"present\":true,\"bytes\":" + quote(String(size)) + ",\"role\":" + quote(fileRole(name)) +
    ",\"url\":" + quote(prefix + session + "/files/" + name) +
    (name.startsWith("raw-") ? ",\"integrity\":\"chunk_crc32\"" : "") + "}";
}

void file(const String &session, const String &name);

void manifest(const String &session, const String &prefix) {
  if (!available()) { error(409, "capture_active"); return; }
  if (!safeSession(session)) { error(400, "invalid_session"); return; }
  const String physicalPath = "/" + session + "/manifest.json";
  File physical = SD.open(physicalPath, FILE_READ);
  const bool physicalAvailable = physical && !physical.isDirectory();
  if (physical) physical.close();
  if (physicalAvailable) {
    file(session, "manifest.json");
    return;
  }
  const bool paused = pauseObservation();
  const String base = "/" + session;
  File check = SD.open(base);
  if (!check || !check.isDirectory()) {
    if (check) check.close(); error(404, "session_not_found"); resumeObservation(paused); return;
  }
  check.close();
  const String start = smallFile(base + "/session-start.json");
  const String result = smallFile(base + "/test-result.json");
  String json; json.reserve(7000);
  json = "{\"api_version\":\"1.0\",\"folder\":" + quote(session) +
    ",\"session_id\":" + quote(scalar(start, "session_id").length() ? scalar(start, "session_id") : session) +
    ",\"state\":" + quote(result.length() ? (scalar(result, "lifecycle").length() ? scalar(result, "lifecycle") : "CLOSED") :
      (start.length() ? "INTERRUPTED" : "UNKNOWN")) +
    ",\"schema_version\":" + quote(scalar(start, "schema_version")) +
    ",\"session_start\":" + (start.length() ? start : "null") +
    ",\"test_result\":" + (result.length() ? result : "null") + ",\"files\":{";
  static constexpr const char *required[] = {"segments.jsonl", "chunks.jsonl", "chunk-index.jsonl", "events.jsonl"};
  for (uint8_t i = 0; i < 4; ++i) {
    if (i) json += ',';
    json += quote(required[i]) + ":" + fileDescriptor(base, session, required[i], prefix);
  }
  json += "},\"file_list\":[";
  bool first = true; File directory = SD.open(base);
  if (directory) {
    for (File file = directory.openNextFile(); file; file = directory.openNextFile()) {
      String name = file.name(); const int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
      if (!file.isDirectory() && allowedFile(name)) {
        if (!first) json += ','; first = false;
        json += "{\"name\":" + quote(name) + ",\"bytes\":" + quote(String(file.size())) +
          ",\"role\":" + quote(fileRole(name)) + ",\"url\":" + quote(prefix + session + "/files/" + name) +
          (name.startsWith("raw-") ? ",\"integrity\":\"chunk_crc32\"" : "") + "}";
      }
      file.close();
    }
    directory.close();
  }
  json += "],\"useful_bytes_approx\":" + quote(String(directoryBytes(base))) + "}";
  sendJson(200, json);
  resumeObservation(paused);
}

void file(const String &session, const String &name) {
  if (!available()) { error(409, "capture_active"); return; }
  if (!safeSession(session) || !safeLeaf(name) || !allowedFile(name)) { error(400, "invalid_path"); return; }
  const bool paused = pauseObservation();
  File input = SD.open("/" + session + "/" + name, FILE_READ);
  if (!input || input.isDirectory()) {
    if (input) input.close(); error(404, "file_not_found"); resumeObservation(paused); return;
  }
  const uint64_t size = input.size();
  uint64_t start = 0, end = size ? size - 1 : 0; bool partial = false, invalid = false;
  const String range = server.header("Range");
  if (range.length()) {
    if (!range.startsWith("bytes=")) invalid = true;
    else {
      const String spec = range.substring(6); const int dash = spec.indexOf('-');
      if (dash <= 0 || spec.indexOf(',', dash) >= 0) invalid = true;
      else {
        start = strtoull(spec.substring(0, dash).c_str(), nullptr, 10);
        const String last = spec.substring(dash + 1);
        end = last.length() ? strtoull(last.c_str(), nullptr, 10) : size - 1;
        if (!size || start >= size || end < start) invalid = true;
        else { if (end >= size) end = size - 1; partial = true; }
      }
    }
  }
  if (invalid) {
    input.close(); server.sendHeader("Content-Range", "bytes */" + String(size));
    error(416, "range_not_satisfiable"); resumeObservation(paused); return;
  }
  const uint64_t length = size ? end - start + 1 : 0;
  if (start && !input.seek(start)) {
    input.close(); error(500, "seek_failed"); resumeObservation(paused); return;
  }
  server.sendHeader("Accept-Ranges", "bytes"); server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Connection", "close");
  if (partial) server.sendHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(size));
  server.setContentLength(length); server.send(partial ? 206 : 200, name.endsWith(".json") ? "application/json" :
    name.endsWith(".jsonl") ? "application/x-ndjson" : "application/octet-stream", "");
  WiFiClient client = server.client(); client.setNoDelay(true);
  uint64_t sent = 0;
  while (sent < length && client.connected()) {
    const size_t wanted = size_t(min<uint64_t>(sizeof(transferBuffer), length - sent));
    const int count = input.read(transferBuffer, wanted);
    if (count <= 0 || !writeAll(client, transferBuffer, count)) { ++transferErrors; break; }
    sent += count; yield();
  }
  input.close(); transferBytes += sent; ++transferCount;
  client.stop();
  resumeObservation(paused);
}

bool split(const String &uri, String &session, String &tail, String &prefix) {
  if (uri.startsWith("/api/v1/sessions/")) prefix = "/api/v1/sessions/";
  else if (uri.startsWith("/api/sessions/")) prefix = "/api/sessions/";
  else return false;
  const int slash = uri.indexOf('/', prefix.length());
  if (slash < 0) { session = uri.substring(prefix.length()); tail = ""; }
  else { session = uri.substring(prefix.length(), slash); tail = uri.substring(slash + 1); }
  return safeSession(session);
}

bool dispatch() {
  String session, tail, prefix;
  if (!split(server.uri(), session, tail, prefix)) return false;
  if (tail.isEmpty() || tail == "manifest") { manifest(session, prefix); return true; }
  if (tail.startsWith("files/")) { file(session, tail.substring(6)); return true; }
  error(404, "endpoint_not_found"); return true;
}

} // namespace sessionapi
