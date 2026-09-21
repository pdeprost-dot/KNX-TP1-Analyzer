#include "web_server.h"
#include "web_ui.h"
#include "analysis.h"
#include "acquisition.h"
#include "events.h"
#include "storage.h"
#include "wifi_manager.h"
#include "hmi.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

namespace webui {
namespace {
WebServer server(80);
WebSocketsServer sockets(81);
uint32_t lastBroadcastMs = 0;
bool sdReady = false;
uint32_t httpErrors = 0;

String encodeStatus() {
  const auto a = analysis::status();
  const auto adc = scope::status();
  const auto wifi = network::status();
  JsonDocument doc;
  doc["firmware"] = "0.5.0-sessions";
  doc["uptime_ms"] = millis();
  doc["flash_bytes"] = ESP.getFlashChipSize();
  doc["analysis"]["state"] = analysis::name(a.state);
  doc["analysis"]["session"] = a.session;
  doc["analysis"]["session_id"] = storage::currentSessionId()[0] ? storage::currentSessionId() : storage::lastSessionId();
  doc["analysis"]["session_elapsed_ms"] = a.sessionElapsedMs;
  doc["analysis"]["session_samples"] = a.sessionSamples;
  doc["adc"]["ready"] = adc.initialized;
  doc["adc"]["running"] = adc.running;
  doc["adc"]["captured"] = adc.captured;
  doc["adc"]["requested_hz"] = adc.requestedHz;
  doc["adc"]["measured_hz"] = adc.measuredHz;
  doc["adc"]["samples"] = adc.samples;
  doc["adc"]["overruns"] = adc.overruns;
  doc["adc"]["read_errors"] = adc.readErrors;
  doc["adc"]["invalid"] = adc.invalid;
  doc["adc"]["ring_valid"] = adc.ringValid;
  doc["adc"]["ring_wraps"] = adc.ringWraps;
  doc["adc"]["latest_raw"] = adc.latestRaw;
  doc["heap"]["free"] = ESP.getFreeHeap();
  doc["heap"]["minimum"] = ESP.getMinFreeHeap();
  doc["heap"]["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  doc["wifi"]["mode"] = network::modeName(wifi.mode);
  doc["wifi"]["ssid"] = wifi.ssid;
  doc["wifi"]["ip"] = wifi.ip;
  doc["wifi"]["rssi"] = wifi.rssi;
  doc["wifi"]["ap_ssid"] = wifi.apSsid;
  doc["wifi"]["primary_configured"] = wifi.primaryConfigured;
  doc["wifi"]["backup_configured"] = wifi.backupConfigured;
  const auto sd = storage::status();
  doc["sd"]["ready"] = sd.mounted;
  doc["sd"]["detected"] = sd.detected;
  doc["sd"]["mounted"] = sd.mounted;
  doc["sd"]["type"] = sd.cardType;
  doc["sd"]["card_bytes"] = sd.cardBytes;
  doc["sd"]["total_bytes"] = sd.totalBytes;
  doc["sd"]["free_bytes"] = sd.freeBytes;
  doc["sd"]["write_errors"] = sd.writeErrors;
  doc["sd"]["queue_depth"] = sd.queueDepth;
  doc["sd"]["queue_high_water"] = sd.queueHighWater;
  doc["sd"]["captures_persisted"] = sd.capturesPersisted;
  doc["sd"]["captures_failed"] = sd.capturesFailed;
  doc["sd"]["bytes_written"] = sd.bytesWritten;
  doc["sd"]["current_session"] = sd.currentSession;
  doc["sd"]["last_session"] = sd.lastSession;
  doc["knx"] = "NOT_CONNECTED";
  doc["vbus"] = "NOT_CONNECTED";
  doc["events"] = events::count();
  doc["http"]["errors"] = httpErrors;
  String output;
  serializeJson(doc, output);
  return output;
}

void json(int code, const String &body) {
  if (code >= 400) ++httpErrors;
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

JsonDocument requestBody() {
  JsonDocument doc;
  deserializeJson(doc, server.arg("plain"));
  return doc;
}

void apiScopeStatus() {
  const auto adc = scope::status();
  JsonDocument doc;
  doc["state"] = adc.captured ? "CAPTURED" : adc.running ? "ARMED" : "STOPPED";
  doc["requested_hz"] = adc.requestedHz;
  doc["measured_hz"] = adc.measuredHz;
  doc["samples"] = adc.samples;
  doc["ring_valid"] = adc.ringValid;
  doc["ring_capacity"] = scope::kRingSamples;
  doc["pre_samples"] = scope::kPreSamples;
  doc["post_samples"] = scope::kPostSamples;
  doc["trigger_threshold_raw"] = scope::kTriggerThresholdRaw;
  doc["capture_number"] = adc.captureNumber;
  doc["overruns"] = adc.overruns;
  doc["min_raw"] = adc.captureMin;
  doc["max_raw"] = adc.captureMax;
  String output;
  serializeJson(doc, output);
  json(200, output);
}

void sendWaveform(const scope::Waveform &wave, bool available, uint32_t sampleRate,
                  const char *state, const char *source) {
  String output;
  output.reserve(3600);
  output += "{\"state\":\"";
  output += state;
  output += "\",\"raw_source\":\"";
  output += source;
  output += "\",\"samples\":";
  output += available ? wave.samples : 0;
  output += ",\"sample_rate_hz\":";
  output += sampleRate;
  output += ",\"pre_samples\":";
  output += available && wave.captured ? scope::kPreSamples : 0;
  output += ",\"post_samples\":";
  output += available && wave.captured ? scope::kPostSamples : 0;
  output += ",\"trigger_position\":";
  output += available && wave.captured ? scope::kPreSamples : 0;
  output += ",\"min_raw\":";
  output += available ? wave.minRaw : 0;
  output += ",\"max_raw\":";
  output += available ? wave.maxRaw : 0;
  output += ",\"low\":[";
  if (available) for (uint16_t i = 0; i < scope::kWaveColumns; ++i) {
    if (i) output += ',';
    output += wave.low[i];
  }
  output += "],\"high\":[";
  if (available) for (uint16_t i = 0; i < scope::kWaveColumns; ++i) {
    if (i) output += ',';
    output += wave.high[i];
  }
  output += "]}";
  json(200, output);
}
void apiWaveform() {
  scope::Waveform wave = {};
  const bool available = scope::waveform(wave);
  const auto adc = scope::status();
  uint32_t captureHz = adc.measuredHz;
  events::Event latestEvent;
  if (adc.captured && events::newest(0, latestEvent) && latestEvent.captureNumber == adc.captureNumber)
    captureHz = latestEvent.sampleRate;
  sendWaveform(wave, available, captureHz,
               adc.captured ? "CAPTURED" : adc.running ? "ARMED" : "STOPPED",
               adc.captured ? "RAM" : "LIVE");
}
void apiStoredWaveform(const String &sessionId, uint32_t eventId) {
  if (scope::status().running) { json(409, "{\"error\":\"adc_busy_retry\"}"); return; }
  scope::Waveform wave = {};
  uint32_t sampleRate = 0;
  if (!storage::readWaveform(sessionId, eventId, wave, sampleRate)) {
    json(422, "{\"error\":\"stored_capture_invalid\"}"); return;
  }
  sendWaveform(wave, true, sampleRate, "CAPTURED", "SD");
}

void apiWifiStatus() {
  const auto wifi = network::status();
  JsonDocument doc;
  doc["mode"] = network::modeName(wifi.mode);
  doc["ssid"] = wifi.ssid;
  doc["ip"] = wifi.ip;
  doc["rssi"] = wifi.rssi;
  doc["ap_ssid"] = wifi.apSsid;
  doc["primary_configured"] = wifi.primaryConfigured;
  doc["backup_configured"] = wifi.backupConfigured;
  String output;
  serializeJson(doc, output);
  json(200, output);
}

void apiWifiNetworks() {
  if (server.hasArg("refresh")) network::startScan();
  int count = network::scanComplete();
  if (count == -2) { network::startScan(); count = -1; }
  JsonDocument doc;
  doc["scanning"] = count < 0;
  JsonArray networks = doc["networks"].to<JsonArray>();
  if (count > 0) for (int i = 0; i < count && i < 24; ++i) {
    JsonObject item = networks.add<JsonObject>();
    item["ssid"] = WiFi.SSID(i);
    item["rssi"] = WiFi.RSSI(i);
  }
  String output;
  serializeJson(doc, output);
  json(200, output);
}

void eventJson(JsonObject node, const events::Event &event) {
  node["event_id"] = event.eventId;
  node["session_id"] = event.sessionId;
  node["uptime_ms"] = event.uptimeMs;
  node["trigger"] = events::triggerName(event.triggerType);
  node["sample_rate_hz"] = event.sampleRate;
  node["sample_count"] = event.sampleCount;
  node["trigger_index"] = event.triggerIndex;
  node["pre_trigger_samples"] = event.preTriggerSamples;
  node["post_trigger_samples"] = event.postTriggerSamples;
  node["adc_min"] = event.adcMin;
  node["adc_max"] = event.adcMax;
  node["adc_mean_before"] = event.adcMeanBefore;
  node["adc_mean_after"] = event.adcMeanAfter;
  node["capture_state"] = events::captureStateName(event.captureState);
  node["raw_source"] = events::rawAvailable(event) ? "RAM" :
    event.rawPersisted ? "SD" : "unavailable";
}

void apiEventsList() {
  JsonDocument doc;
  doc["total"] = events::count();
  doc["retained"] = events::retainedCount();
  doc["capacity"] = events::kCapacity;
  JsonArray items = doc["events"].to<JsonArray>();
  events::Event event;
  for (uint8_t i = 0; events::newest(i, event); ++i) {
    eventJson(items.add<JsonObject>(), event);
  }
  String output;
  serializeJson(doc, output);
  json(200, output);
}

bool apiEventRoute() {
  const String uri = server.uri();
  constexpr char prefix[] = "/api/events/";
  if (!uri.startsWith(prefix)) return false;
  String remainder = uri.substring(sizeof(prefix) - 1);
  const bool capture = remainder.endsWith("/capture");
  if (capture) remainder.remove(remainder.length() - 8);
  if (remainder.isEmpty()) { json(404, "{\"error\":\"event_not_found\"}"); return true; }
  for (size_t i = 0; i < remainder.length(); ++i) {
    if (!isDigit(remainder[i])) { json(404, "{\"error\":\"event_not_found\"}"); return true; }
  }
  const uint32_t id = static_cast<uint32_t>(remainder.toInt());
  events::Event event;
  if (id == 0 || !events::find(id, event)) {
    json(404, "{\"error\":\"event_not_found\"}");
  } else if (capture && events::rawAvailable(event)) {
    apiWaveform();
  } else if (capture && event.rawPersisted) {
    apiStoredWaveform(event.sessionId, id);
  } else if (capture) {
    json(410, "{\"error\":\"raw_capture_not_retained\",\"event_id\":" + String(id) + "}");
  } else {
    JsonDocument doc;
    eventJson(doc.to<JsonObject>(), event);
    String output;
    serializeJson(doc, output);
    json(200, output);
  }
  return true;
}
const char *sessionRawSource(const String &id, uint32_t eventId) {
  events::Event current;
  if (events::find(eventId, current) && strcmp(current.sessionId, id.c_str()) == 0 && events::rawAvailable(current))
    return "RAM";
  return storage::hasCapture(id, eventId) ? "SD" : "unavailable";
}
void apiSessionsList() {
  if (scope::status().running) { json(409, "{\"error\":\"adc_busy_retry\"}"); return; }
  JsonDocument doc;
  if (!storage::listSessions(doc)) { json(503, "{\"error\":\"sd_unavailable\"}"); return; }
  String output;
  serializeJson(doc, output);
  json(200, output);
}
bool parseEventId(const String &value, uint32_t &id) {
  if (value.isEmpty() || value.length() > 10) return false;
  for (size_t i = 0; i < value.length(); ++i) if (!isDigit(value[i])) return false;
  id = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
  return id != 0;
}
bool apiSessionRoute() {
  const String uri = server.uri();
  constexpr char prefix[] = "/api/sessions/";
  if (!uri.startsWith(prefix)) return false;
  if (scope::status().running) { json(409, "{\"error\":\"adc_busy_retry\"}"); return true; }
  String rest = uri.substring(sizeof(prefix) - 1);
  const int slash = rest.indexOf('/');
  const String id = slash < 0 ? rest : rest.substring(0, slash);
  if (!storage::validSessionId(id)) { json(404, "{\"error\":\"session_not_found\"}"); return true; }
  const String tail = slash < 0 ? "" : rest.substring(slash);
  JsonDocument doc;
  if (tail.isEmpty()) {
    if (!storage::readSession(id, doc)) { json(404, "{\"error\":\"session_not_found\"}"); return true; }
  } else if (tail == "/events") {
    if (!storage::listEvents(id, doc)) { json(404, "{\"error\":\"session_not_found\"}"); return true; }
    doc["session_id"] = id;
    for (JsonObject item : doc["events"].as<JsonArray>())
      item["raw_source"] = sessionRawSource(id, item["event_id"].as<uint32_t>());
  } else if (tail.startsWith("/events/")) {
    String eventPart = tail.substring(8);
    const bool capture = eventPart.endsWith("/capture");
    const bool raw = eventPart.endsWith("/raw");
    if (capture) eventPart.remove(eventPart.length() - 8);
    if (raw) eventPart.remove(eventPart.length() - 4);
    uint32_t eventId = 0;
    if (!parseEventId(eventPart, eventId) || !storage::readEvent(id, eventId, doc)) {
      json(404, "{\"error\":\"event_not_found\"}"); return true;
    }
    if (capture) {
      events::Event current;
      if (events::find(eventId, current) && strcmp(current.sessionId, id.c_str()) == 0 && events::rawAvailable(current))
        apiWaveform();
      else if (storage::hasCapture(id, eventId)) apiStoredWaveform(id, eventId);
      else json(410, "{\"error\":\"raw_capture_unavailable\"}");
      return true;
    }
    if (raw) {
      File file = storage::openRaw(id, eventId);
      if (!file) { json(410, "{\"error\":\"raw_capture_unavailable\"}"); return true; }
      server.sendHeader("Cache-Control", "no-store");
      server.streamFile(file, "application/octet-stream");
      file.close();
      return true;
    }
    doc["raw_source"] = sessionRawSource(id, eventId);
  } else {
    json(404, "{\"error\":\"not_found\"}"); return true;
  }
  String output;
  serializeJson(doc, output);
  json(200, output);
  return true;
}

}

void setSdReady(bool ready) { sdReady = ready; }
unsigned long errorCount() { return httpErrors; }

void begin() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", kWebPage); });
  server.on("/sessions", HTTP_GET, [] { server.send_P(200, "text/html", kWebPage); });
  server.on("/api/status", HTTP_GET, [] { json(200, encodeStatus()); });
  server.on("/api/analysis/start", HTTP_POST, [] {
    if (storage::busy()) { json(409, "{\"error\":\"sd_capture_pending\"}"); return; }
    const bool okay = analysis::start();
    hmi::refresh();
    json(okay ? 200 : 500, okay ? "{\"state\":\"RUNNING\"}" : "{\"error\":\"start_failed\"}");
  });
  server.on("/api/analysis/stop", HTTP_POST, [] {
    const bool okay = analysis::stop();
    hmi::refresh();
    json(okay ? 200 : 500, okay ? "{\"state\":\"STOPPED\"}" : "{\"error\":\"stop_failed\"}");
  });
  server.on("/api/scope/status", HTTP_GET, apiScopeStatus);
  server.on("/api/events", HTTP_GET, apiEventsList);
  server.on("/api/sessions", HTTP_GET, apiSessionsList);
  server.on("/api/scope/capture", HTTP_GET, apiWaveform);
  server.on("/api/scope/arm", HTTP_POST, [] {
    if (analysis::status().state != analysis::State::Running) {
      json(409, "{\"error\":\"analysis_stopped\"}"); return;
    }
    JsonDocument body = requestBody();
    String selected = body["mode"] | "manual";
    scope::TriggerMode trigger = selected == "rising" ? scope::TriggerMode::Rising
      : selected == "falling" ? scope::TriggerMode::Falling : scope::TriggerMode::Manual;
    if (storage::busy()) { json(409, "{\"error\":\"sd_capture_pending\"}"); return; }
    const bool okay = scope::arm(trigger) && scope::start();
    hmi::refresh();
    json(okay ? 200 : 500, okay ? "{\"state\":\"ARMED\"}" : "{\"error\":\"arm_failed\"}");
  });
  server.on("/api/scope/trigger", HTTP_POST, [] {
    const bool okay = scope::manualTrigger();
    json(okay ? 202 : 409, okay ? "{\"state\":\"TRIGGER_PENDING\"}" : "{\"error\":\"not_armed_or_prebuffer_empty\"}");
  });
  server.on("/api/scope/clear", HTTP_POST, [] { scope::clear(); hmi::refresh(); json(200, "{\"cleared\":true}"); });
  server.on("/api/wifi/status", HTTP_GET, apiWifiStatus);
  server.on("/api/wifi/networks", HTTP_GET, apiWifiNetworks);
  server.on("/api/wifi/profiles", HTTP_POST, [] {
    JsonDocument body = requestBody();
    const int slot = body["slot"] | 0;
    const String ssid = body["ssid"] | "";
    const String password = body["password"] | "";
    if (slot < 1 || slot > 2 || !network::saveProfile(slot - 1, ssid, password)) {
      json(400, "{\"error\":\"invalid_profile\"}"); return;
    }
    json(200, "{\"saved\":true,\"connecting\":true}");
  });
  server.onNotFound([] {
    if (server.method() == HTTP_GET && apiEventRoute()) return;
    if (server.method() == HTTP_GET && apiSessionRoute()) return;
    if (server.uri().startsWith("/api/")) json(404, "{\"error\":\"not_found\"}");
    else if (network::status().mode == network::Mode::AccessPoint) {
      server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", "");
    } else server.send(404, "text/plain", "Not found");
  });
  server.begin();
  sockets.begin();
  sockets.enableHeartbeat(15000, 3000, 2);
  Serial.println("{\"type\":\"WEB_STATUS\",\"http\":80,\"websocket\":81,\"ready\":true}");
}

void tick() {
  server.handleClient();
  sockets.loop();
  if (millis() - lastBroadcastMs >= 1000) {
    lastBroadcastMs = millis();
    if (sockets.connectedClients() > 0) {
      String message = "{\"type\":\"status\",\"data\":" + encodeStatus() + "}";
      sockets.broadcastTXT(message);
    }
  }
}
}
