#include "web_server.h"
#include "web_ui.h"
#include "analysis.h"
#include "acquisition.h"
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

String encodeStatus() {
  const auto a = analysis::status();
  const auto adc = scope::status();
  const auto wifi = network::status();
  JsonDocument doc;
  doc["firmware"] = "0.3.0-hmi";
  doc["uptime_ms"] = millis();
  doc["flash_bytes"] = ESP.getFlashChipSize();
  doc["analysis"]["state"] = analysis::name(a.state);
  doc["analysis"]["session"] = a.session;
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
  doc["sd"]["ready"] = sdReady;
  doc["knx"] = "NOT_CONNECTED";
  doc["vbus"] = "NOT_CONNECTED";
  doc["events"] = 0;
  String output;
  serializeJson(doc, output);
  return output;
}

void json(int code, const String &body) {
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

void apiWaveform() {
  scope::Waveform wave = {};
  const bool available = scope::waveform(wave);
  const auto adc = scope::status();
  String output;
  output.reserve(3500);
  output += "{\"state\":\"";
  output += adc.captured ? "CAPTURED" : adc.running ? "ARMED" : "STOPPED";
  output += "\",\"samples\":";
  output += available ? wave.samples : 0;
  output += ",\"sample_rate_hz\":";
  output += adc.measuredHz;
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
}

void setSdReady(bool ready) { sdReady = ready; }

void begin() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", kWebPage); });
  server.on("/api/status", HTTP_GET, [] { json(200, encodeStatus()); });
  server.on("/api/analysis/start", HTTP_POST, [] {
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
  server.on("/api/scope/capture", HTTP_GET, apiWaveform);
  server.on("/api/scope/arm", HTTP_POST, [] {
    if (analysis::status().state != analysis::State::Running) {
      json(409, "{\"error\":\"analysis_stopped\"}"); return;
    }
    JsonDocument body = requestBody();
    String selected = body["mode"] | "manual";
    scope::TriggerMode trigger = selected == "rising" ? scope::TriggerMode::Rising
      : selected == "falling" ? scope::TriggerMode::Falling : scope::TriggerMode::Manual;
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
