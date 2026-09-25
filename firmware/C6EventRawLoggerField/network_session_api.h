#pragma once

String nsQuote(const String &v) { String o; o += char(34); o += v; o += char(34); return o; }
void nsError(int code, const char *value) { server.send(code, "application/json", "{" + nsQuote("error") + ":" + nsQuote(value) + "}"); }
String nsSmallFile(const String &path, size_t limit = 16384) {
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory() || f.size() > limit) { if (f) f.close(); return String(); }
  String s; s.reserve(f.size() + 1); while (f.available()) s += char(f.read()); f.close(); s.trim(); return s;
}
String nsScalar(const String &json, const char *key) {
  String n = nsQuote(key) + ":"; int p = json.indexOf(n); if (p < 0) return String(); p += n.length();
  while (p < int(json.length()) && json[p] == ' ') ++p;
  if (p >= int(json.length())) return String();
  if (json[p] == char(34)) { int e = json.indexOf(char(34), p + 1); return e > p ? json.substring(p + 1, e) : String(); }
  int e = p; while (e < int(json.length()) && json[e] != ',' && json[e] != '}') ++e; return json.substring(p, e);
}
bool nsSafeLeaf(const String &v) {
  if (v.isEmpty() || v == "." || v == ".." || v.indexOf('/') >= 0 || v.indexOf(char(92)) >= 0) return false;
  for (char c : v) if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
  return true;
}
bool nsSafeSession(const String &v) { return nsSafeLeaf(v) && v.startsWith("EVENT-"); }
String nsContentType(const String &n) { if (n.endsWith(".json")) return "application/json"; if (n.endsWith(".jsonl")) return "application/x-ndjson"; return "application/octet-stream"; }
uint64_t nsUsefulBytes(const String &base) {
  uint64_t n = 0; File d = SD.open(base); if (!d) return 0;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) { if (!f.isDirectory()) n += f.size(); f.close(); }
  d.close(); return n;
}
void nsAnalyzer() {
  FieldSnapshot s = fieldSnapshot(); String o = "{";
  o += nsQuote("api_version") + ":" + nsQuote(networkApiVersion);
  o += "," + nsQuote("analyzer_id") + ":" + nsQuote(ls1AnalyzerId());
  o += "," + nsQuote("hostname") + ":" + nsQuote(hostName);
  o += "," + nsQuote("firmware_version") + ":" + nsQuote(firmwareVersion);
  o += "," + nsQuote("logger_schema") + ":" + nsQuote("knx-long-session-1.0");
  o += "," + nsQuote("raw_format") + ":" + nsQuote("segmented-v1");
  o += "," + nsQuote("state") + ":" + nsQuote(stateName(s.state));
  o += "," + nsQuote("ip") + ":" + nsQuote(WiFi.localIP().toString());
  o += "," + nsQuote("rssi") + ":" + String(WiFi.RSSI());
  o += "," + nsQuote("sd_ready") + ":" + String(s.sdReady ? "true" : "false");
  o += "," + nsQuote("ota_available") + ":" + String((s.state == FieldState::Idle || s.state == FieldState::Closed) && !otaActive ? "true" : "false") + "}";
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", o);
}
void nsSessions() {
  String o = "{" + nsQuote("api_version") + ":" + nsQuote(networkApiVersion) + "," + nsQuote("sessions") + ":["; bool first = true; File root = SD.open("/");
  if (root) { for (File d = root.openNextFile(); d; d = root.openNextFile()) { String name = d.name(); if (name.startsWith("/")) name.remove(0, 1);
    if (d.isDirectory() && nsSafeSession(name)) { String base = "/" + name, start = nsSmallFile(base + "/session-start.json"), result = nsSmallFile(base + "/test-result.json"); String state = result.length() ? "CLOSED" : start.length() ? "INTERRUPTED" : "UNKNOWN";
      if (!first) o += ','; first = false; o += "{" + nsQuote("folder") + ":" + nsQuote(name);
      o += "," + nsQuote("session_id") + ":" + nsQuote(nsScalar(start, "session_id"));
      o += "," + nsQuote("state") + ":" + nsQuote(state);
      o += "," + nsQuote("schema_version") + ":" + nsQuote(nsScalar(start, "schema_version"));
      String events = nsScalar(result, "events_total"); o += "," + nsQuote("events") + ":" + (events.length() ? events : String("null"));
      o += "," + nsQuote("valid_raw_bytes") + ":" + nsQuote(nsScalar(result, "raw_bytes"));
      o += "," + nsQuote("duration_us") + ":" + nsQuote(nsScalar(result, "duration_us"));
      o += "," + nsQuote("useful_bytes_approx") + ":" + nsQuote(String(nsUsefulBytes(base))) + "}";
    } d.close(); } root.close(); }
  o += "]}"; server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", o);
}
bool nsSplit(const String &uri, String &session, String &tail) {
  const String prefix = "/api/sessions/"; if (!uri.startsWith(prefix)) return false; int slash = uri.indexOf('/', prefix.length()); if (slash < 0) return false;
  session = uri.substring(prefix.length(), slash); tail = uri.substring(slash + 1); return nsSafeSession(session);
}
void nsManifest(const String &session) {
  String base = "/" + session; if (!SD.exists(base)) { nsError(404, "session_not_found"); return; }
  String start = nsSmallFile(base + "/session-start.json"), result = nsSmallFile(base + "/test-result.json"); String state = result.length() ? "CLOSED" : start.length() ? "INTERRUPTED" : "UNKNOWN";
  String o = "{" + nsQuote("api_version") + ":" + nsQuote(networkApiVersion) + "," + nsQuote("folder") + ":" + nsQuote(session) + "," + nsQuote("state") + ":" + nsQuote(state);
  o += "," + nsQuote("session_start") + ":" + (start.length() ? start : String("null")); o += "," + nsQuote("test_result") + ":" + (result.length() ? result : String("null")); o += "," + nsQuote("files") + ":{";
  const char *names[] = {"segments.jsonl", "chunks.jsonl", "chunk-index.jsonl", "events.jsonl"};
  for (uint8_t i = 0; i < 4; ++i) { if (i) o += ','; String p = base + "/" + names[i]; File f = SD.open(p, FILE_READ); o += nsQuote(names[i]) + ":";
    if (f) { o += "{" + nsQuote("present") + ":true," + nsQuote("bytes") + ":" + nsQuote(String(f.size())) + "," + nsQuote("url") + ":" + nsQuote("/api/sessions/" + session + "/files/" + names[i]) + "}"; f.close(); }
    else o += "{" + nsQuote("present") + ":false}";
  }
  o += "}}"; server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", o);
}
void nsFile(const String &session, const String &leaf) {
  if (!nsSafeLeaf(leaf)) { nsError(400, "invalid_path"); return; }
  if (!transferAllowed()) { nsError(409, "capture_active"); return; }
  File f = SD.open("/" + session + "/" + leaf, FILE_READ); if (!f || f.isDirectory()) { if (f) f.close(); nsError(404, "file_not_found"); return; }
  uint64_t size = f.size(), start = 0, end = size ? size - 1 : 0; bool partial = false, invalid = false; String range = server.header("Range");
  if (range.length()) { if (!range.startsWith("bytes=")) invalid = true; else { String spec = range.substring(6); int dash = spec.indexOf('-'); if (dash <= 0) invalid = true; else { start = strtoull(spec.substring(0, dash).c_str(), nullptr, 10); String last = spec.substring(dash + 1); end = last.length() ? strtoull(last.c_str(), nullptr, 10) : size - 1; if (start >= size || end < start) invalid = true; else { if (end >= size) end = size - 1; partial = true; } } } }
  if (invalid) { f.close(); server.sendHeader("Content-Range", "bytes */" + String(size)); nsError(416, "range_not_satisfiable"); return; }
  uint64_t length = size ? end - start + 1 : 0; if (!f.seek(start)) { f.close(); nsError(500, "seek_failed"); return; }
  uint8_t *buffer = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_8BIT); if (!buffer) { f.close(); nsError(503, "buffer_allocation_failed"); return; }
  resetTransferStats(8192, false); transferStats.requestedBytes = length; WiFiClient client = server.client(); client.setNoDelay(true);
  server.sendHeader("Accept-Ranges", "bytes"); server.sendHeader("Cache-Control", "no-store"); server.sendHeader("Connection", "close"); if (partial) server.sendHeader("Content-Range", "bytes " + String(start) + "-" + String(end) + "/" + String(size)); server.setContentLength(length); server.send(partial ? 206 : 200, nsContentType(leaf), "");
  uint64_t began = esp_timer_get_time(); while (transferStats.bytes < length) { uint32_t want = uint32_t(min<uint64_t>(8192, length - transferStats.bytes)); uint64_t t = esp_timer_get_time(); int n = f.read(buffer, want); transferStats.sdReadUs += esp_timer_get_time() - t; if (n <= 0) { ++transferStats.sdErrors; break; } transferStats.crc = esp_crc32_le(transferStats.crc, buffer, n); if (!writeAll(client, buffer, n)) break; uint32_t heap = ESP.getFreeHeap(); if (heap < transferStats.heapMin) transferStats.heapMin = heap; yield(); }
  transferStats.durationUs = esp_timer_get_time() - began; transferStats.heapEnd = ESP.getFreeHeap(); transferStats.complete = transferStats.bytes == length && transferStats.sdErrors == 0 && transferStats.networkErrors == 0; f.close(); free(buffer); client.stop();
}
bool nsDispatch() {
  String session, tail; if (!nsSplit(server.uri(), session, tail)) return false;
  if (tail == "manifest") { nsManifest(session); return true; }
  if (tail.startsWith("files/")) { nsFile(session, tail.substring(6)); return true; }
  nsError(404, "endpoint_not_found"); return true;
}
