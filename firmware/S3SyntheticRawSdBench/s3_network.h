#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_wifi.h>

#ifndef S3_WIFI_MODE
#define S3_WIFI_MODE 3
#endif
#ifndef S3_HTTP_ENABLED
#define S3_HTTP_ENABLED 1
#endif
#ifndef S3_MDNS_ENABLED
#define S3_MDNS_ENABLED 1
#endif
#ifndef S3_OTA_ENABLED
#define S3_OTA_ENABLED 1
#endif
#ifndef S3_STA_BEHAVIOR
#define S3_STA_BEHAVIOR 2
#endif
#ifndef S3_WIFI_POWER_SAVE_NONE
#define S3_WIFI_POWER_SAVE_NONE 0
#endif
#ifndef S3_WIFI_TX_POWER_QDBM
#define S3_WIFI_TX_POWER_QDBM -1
#endif

namespace s3net {
WebServer server(80);
String staSsid[2], staPassword[2], otaPassword;
char apSsid[32] = {}, hostName[40] = {};
uint8_t staProfile = 0, staAttempt = 0;
uint32_t staAttemptMs = 0, staRetryMs = 0;
uint32_t staAttempts = 0, staReconnects = 0, connectionChanges = 0, httpRequests = 0;
volatile uint16_t lastDisconnectReason = 0;
volatile uint32_t disconnectEvents = 0;
wl_status_t previousStaStatus = WL_IDLE_STATUS;
bool everConnected = false, otaActive = false, otaServiceRunning = false, mdnsReady = false;
constexpr uint32_t STA_ATTEMPT_TIMEOUT_MS = 12000, STA_RETRY_INTERVAL_MS = 30000;

const char *networkStateName() {
  return WiFi.status() == WL_CONNECTED ? "CONNECTED" : staAttempt ? "CONNECTING" : "DISCONNECTED";
}

String statusJson() {
  const State current = state.load();
  const uint64_t end = (current == State::CLOSED || current == State::FAILED) && closedUs ? closedUs : esp_timer_get_time();
  const uint64_t duration = startedUs ? end - startedUs : 0;
  String value; value.reserve(1024);
  value = "{\"state\":\"" + String(stateName(current)) + "\",\"sd_ready\":" + String(sdReady ? "true" : "false");
  value += ",\"duration_us\":\"" + String(duration) + "\",\"chunks\":\"" + String(producedChunks.load());
  value += "\",\"samples\":\"" + String(producedSamples.load()) + "\",\"raw_bytes\":\"" + String(rawBytes.load());
  value += "\",\"R1\":" + String(r1Count.load()) + ",\"raw_handle_failures\":" + String(rawHandleFailures.load());
  value += ",\"retry_failures\":" + String(retryFailures.load()) + ",\"reopen_failures\":" + String(reopenFailures.load());
  value += ",\"sd_errors\":" + String(sdErrors.load()) + ",\"loss\":\"" + String(lostSamples.load());
  value += "\",\"pool_exhaustion\":" + String(poolExhaustion.load()) + ",\"free_min\":" + String(freeMin);
  value += ",\"pending_max\":" + String(pendingMax) + ",\"ready_current\":" + String(readyQ ? uxQueueMessagesWaiting(readyQ) : 0);
  value += ",\"ready_max\":" + String(readyMax) + ",\"write_us_max\":" + String(writeLatencyMaxUs);
  value += ",\"checkpoint_us_max\":" + String(checkpointLatencyMaxUs) + ",\"writer_hold_us_max\":" + String(writerHoldMaxUs);
  value += ",\"crc32\":\""; char crc[9]; snprintf(crc, sizeof(crc), "%08X", sessionCrc); value += crc;
  value += "\",\"invariant\":" + String(invariantOk ? "true" : "false") + ",\"pass\":" + String(finalPass ? "true" : "false");
  value += ",\"sta_state\":\"" + String(networkStateName()) + "\",\"sta_ssid\":\"";
  value += WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
  value += "\",\"sta_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String());
  value += "\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  value += ",\"sta_attempts\":" + String(staAttempts) + ",\"sta_reconnects\":" + String(staReconnects);
  value += ",\"connection_changes\":" + String(connectionChanges) + ",\"http_requests\":" + String(httpRequests) + "}";
  return value;
}

String networkJson() {
  String value; value.reserve(640);
  value = "{\"ap_ssid\":\"" + String(apSsid) + "\",\"ap_ip\":\"" + WiFi.softAPIP().toString();
  value += "\",\"sta_state\":\"" + String(networkStateName()) + "\",\"sta_ssid\":\"";
  value += WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
  value += "\",\"sta_ip\":\"" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String());
  value += "\",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  value += ",\"hostname\":\"" + String(hostName) + "\",\"ssid1\":\"" + staSsid[0] + "\",\"ssid2\":\"" + staSsid[1];
  value += "\",\"ota_available\":" + String((state.load() == State::IDLE || state.load() == State::CLOSED) && !otaActive ? "true" : "false");
  value += ",\"sta_attempts\":" + String(staAttempts) + ",\"sta_reconnects\":" + String(staReconnects);
  value += ",\"connection_changes\":" + String(connectionChanges) + ",\"http_requests\":" + String(httpRequests) + "}";
  return value;
}

void startStaAttempt(uint8_t profile) {
  if (profile > 1 || staSsid[profile].isEmpty()) return;
  if (everConnected) ++staReconnects;
  ++staAttempts; staProfile = profile; staAttempt = profile + 1; staAttemptMs = millis();
  WiFi.begin(staSsid[profile].c_str(), staPassword[profile].c_str());
}
void beginStaCycle() {
  if (!staSsid[0].isEmpty()) startStaAttempt(0);
  else if (!staSsid[1].isEmpty()) startStaAttempt(1);
  else { staAttempt = 0; staRetryMs = millis(); }
}
void tickSta() {
  const wl_status_t current = WiFi.status();
  if (current != previousStaStatus) { ++connectionChanges; previousStaStatus = current; }
  if (current == WL_CONNECTED) { staAttempt = 0; everConnected = true; return; }
  const uint32_t now = millis();
  if (staAttempt && now - staAttemptMs >= STA_ATTEMPT_TIMEOUT_MS) {
    WiFi.disconnect(false, false);
    if (staProfile == 0 && !staSsid[1].isEmpty()) startStaAttempt(1);
    else { staAttempt = 0; staRetryMs = now; }
  } else if (!staAttempt && now - staRetryMs >= STA_RETRY_INTERVAL_MS) beginStaCycle();
}

void loadConfig() {
  Preferences preferences; preferences.begin("s3-net", false);
  staSsid[0] = preferences.getString("ssid1", ""); staPassword[0] = preferences.getString("pass1", "");
  staSsid[1] = preferences.getString("ssid2", ""); staPassword[1] = preferences.getString("pass2", "");
  otaPassword = preferences.getString("ota-pass", "");
  if (otaPassword.length() < 12) {
    char generated[17]; snprintf(generated, sizeof(generated), "%08lX%08lX", (unsigned long)esp_random(), (unsigned long)esp_random());
    otaPassword = generated; preferences.putString("ota-pass", otaPassword);
    Serial.printf("{\"type\":\"OTA_PASSWORD_CREATED\",\"password\":\"%s\"}\n", otaPassword.c_str());
  }
  preferences.end();
}

constexpr char mainPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>S3 RAW/SD Bench</title><style>body{font:16px system-ui;background:#101820;color:#eee;margin:auto;max-width:600px;padding:20px}h1{color:#53d8fb}.card{background:#1c2935;padding:18px;border-radius:14px}pre{white-space:pre-wrap}button{padding:14px;margin:6px;width:45%}</style></head><body><h1>S3 RAW/SD Bench</h1><div class=card><pre id=s>Loading...</pre><button onclick="cmd('start')">START</button><button onclick="cmd('stop')">STOP</button><p><a href=/network>Network configuration</a></p></div><script>async function poll(){try{let d=await fetch('/api/status',{cache:'no-store'}).then(r=>r.json());s.textContent=JSON.stringify(d,null,2)}catch(e){s.textContent=e}setTimeout(poll,500)}async function cmd(v){await fetch('/api/'+v,{method:'POST'})}poll()</script></body></html>)HTML";
constexpr char networkPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>S3 Network</title></head><body><h1>S3 Network</h1><pre id=s></pre><form id=f><label>SSID 1<input name=ssid1 id=s1></label><label>Password 1<input name=pass1 type=password></label><label>SSID 2<input name=ssid2 id=s2></label><label>Password 2<input name=pass2 type=password></label><label>OTA password<input name=otapass type=password minlength=12></label><button>SAVE / APPLY</button></form><script>let loaded=false;async function poll(){let d=await fetch('/api/network',{cache:'no-store'}).then(r=>r.json());s.textContent=JSON.stringify(d,null,2);if(!loaded){s1.value=d.ssid1;s2.value=d.ssid2;loaded=true}setTimeout(poll,1000)}f.onsubmit=async e=>{e.preventDefault();let r=await fetch('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))});alert(await r.text())};poll()</script></body></html>)HTML";

void countRequest() { ++httpRequests; }
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason; ++disconnectEvents;
    Serial.printf("{\"type\":\"WIFI_STA_EVENT\",\"event\":\"DISCONNECTED\",\"reason\":%u}\n", unsigned(info.wifi_sta_disconnected.reason));
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
    Serial.printf("{\"type\":\"WIFI_STA_EVENT\",\"event\":\"GOT_IP\",\"ip\":\"%s\"}\n", WiFi.localIP().toString().c_str());
}
void printDiagnostics() {
  int8_t txPower = 0;
  const esp_err_t txPowerResult = esp_wifi_get_max_tx_power(&txPower);
  Serial.printf("{\"type\":\"WIFI_STATUS\",\"mode\":%u,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
                "\"channel\":%d,\"clients\":%u,\"sta_status\":%d,\"associated\":%s,\"rssi\":%d,"
                "\"tx_power_qdbm\":%d,\"tx_power_get_result\":%d,\"sta_profiles\":%u,"
                "\"disconnect_events\":%u,\"last_disconnect_reason\":%u}\n",
                unsigned(WiFi.getMode()), apSsid, WiFi.softAPIP().toString().c_str(), WiFi.channel(),
                WiFi.softAPgetStationNum(), int(WiFi.status()), WiFi.status() == WL_CONNECTED ? "true" : "false",
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, int(txPower), int(txPowerResult),
                unsigned(!staSsid[0].isEmpty()) + unsigned(!staSsid[1].isEmpty()),
                unsigned(disconnectEvents), unsigned(lastDisconnectReason));
}
void begin() {
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(apSsid, sizeof(apSsid), "S3-RAW-Bench-%04X", uint16_t(mac));
  snprintf(hostName, sizeof(hostName), "s3-raw-bench-%04x", uint16_t(mac));
  loadConfig(); WiFi.onEvent(onWiFiEvent);
  constexpr wifi_mode_t requestedMode = S3_WIFI_MODE == 1 ? WIFI_STA : S3_WIFI_MODE == 2 ? WIFI_AP : WIFI_AP_STA;
  const bool modeReady = WiFi.mode(requestedMode);
#if S3_WIFI_TX_POWER_QDBM >= 0
  const esp_err_t txSetResult = esp_wifi_set_max_tx_power(S3_WIFI_TX_POWER_QDBM);
  int8_t txPowerReadback = 0;
  const esp_err_t txGetResult = esp_wifi_get_max_tx_power(&txPowerReadback);
  Serial.printf("{\"type\":\"WIFI_INIT\",\"step\":\"tx_power\",\"requested_qdbm\":%d,"
                "\"set_result\":%d,\"effective_qdbm\":%d,\"get_result\":%d}\n",
                S3_WIFI_TX_POWER_QDBM, int(txSetResult), int(txPowerReadback), int(txGetResult));
#endif
#if S3_WIFI_POWER_SAVE_NONE
  const esp_err_t powerSaveResult = esp_wifi_set_ps(WIFI_PS_NONE);
  Serial.printf("{\"type\":\"WIFI_INIT\",\"step\":\"power_save\",\"mode\":\"NONE\",\"result\":%d}\n", int(powerSaveResult));
#endif
  WiFi.setHostname(hostName);
  Serial.printf("{\"type\":\"WIFI_INIT\",\"step\":\"mode\",\"requested\":%u,\"result\":%s,\"effective\":%u,\"sta_profiles\":%u}\n",
                unsigned(requestedMode), modeReady ? "true" : "false", unsigned(WiFi.getMode()),
                unsigned(!staSsid[0].isEmpty()) + unsigned(!staSsid[1].isEmpty()));
  bool apReady = false;
#if S3_WIFI_MODE == 2 || S3_WIFI_MODE == 3
  apReady = WiFi.softAP(apSsid);
#endif
  delay(250);
  Serial.printf("{\"type\":\"WIFI_INIT\",\"step\":\"softap\",\"result\":%s,\"ssid\":\"%s\","
                "\"effective_mode\":%u,\"ip\":\"%s\",\"channel\":%d,\"clients\":%u,\"status\":%d}\n",
                apReady ? "true" : "false", apSsid, unsigned(WiFi.getMode()),
                WiFi.softAPIP().toString().c_str(), WiFi.channel(), WiFi.softAPgetStationNum(), int(WiFi.status()));
  #if (S3_WIFI_MODE == 1 || S3_WIFI_MODE == 3) && S3_STA_BEHAVIOR == 2
  beginStaCycle();
  #elif (S3_WIFI_MODE == 1 || S3_WIFI_MODE == 3) && S3_STA_BEHAVIOR == 1
  WiFi.setAutoReconnect(false);
  WiFi.begin("S3-BENCH-NO-ASSOC", "not-a-real-network");
  #endif
#if S3_HTTP_ENABLED
  server.on("/", HTTP_GET, [] { countRequest(); server.send_P(200, "text/html", mainPage); });
  server.on("/network", HTTP_GET, [] { countRequest(); server.send_P(200, "text/html", networkPage); });
  server.on("/api/status", HTTP_GET, [] { countRequest(); server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", statusJson()); });
  server.on("/api/network", HTTP_GET, [] { countRequest(); server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", networkJson()); });
  server.on("/api/network", HTTP_POST, [] {
    countRequest(); Preferences preferences; preferences.begin("s3-net", false);
    String n1 = server.arg("ssid1"), n2 = server.arg("ssid2"), p1 = server.arg("pass1"), p2 = server.arg("pass2"), op = server.arg("otapass");
    if (!op.isEmpty() && op.length() < 12) { preferences.end(); server.send(400, "application/json", "{\"saved\":false,\"error\":\"ota_password_too_short\"}"); return; }
    preferences.putString("ssid1", n1); preferences.putString("ssid2", n2);
    if (n1.isEmpty()) preferences.remove("pass1"); else if (!p1.isEmpty()) preferences.putString("pass1", p1);
    if (n2.isEmpty()) preferences.remove("pass2"); else if (!p2.isEmpty()) preferences.putString("pass2", p2);
    if (!op.isEmpty()) preferences.putString("ota-pass", op); preferences.end();
    staSsid[0] = n1; staSsid[1] = n2;
    if (n1.isEmpty()) staPassword[0] = ""; else if (!p1.isEmpty()) staPassword[0] = p1;
    if (n2.isEmpty()) staPassword[1] = ""; else if (!p2.isEmpty()) staPassword[1] = p2;
    if (!op.isEmpty()) { otaPassword = op; ArduinoOTA.setPassword(otaPassword.c_str()); }
    WiFi.disconnect(false, false); staAttempt = 0; staRetryMs = millis() - STA_RETRY_INTERVAL_MS;
    server.send(200, "application/json", "{\"saved\":true,\"ap_preserved\":true}");
  });
  server.on("/api/start", HTTP_POST, [] { countRequest(); const bool ok = startRun(); server.send(ok ? 202 : 409, "application/json", ok ? "{\"accepted\":true,\"state\":\"RUNNING\"}" : "{\"accepted\":false}"); });
  server.on("/api/stop", HTTP_POST, [] { countRequest(); if (state.load() != State::RUNNING) { server.send(409, "application/json", "{\"accepted\":false}"); return; } server.send(202, "application/json", "{\"accepted\":true,\"state\":\"STOPPING\"}"); finalizeRun(); });
  server.onNotFound([] { countRequest(); server.send(404, "application/json", "{\"error\":\"not_found\"}"); });
  server.begin();
#endif
#if S3_OTA_ENABLED
  ArduinoOTA.setHostname(hostName); ArduinoOTA.setPassword(otaPassword.c_str()); ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([] { otaActive = true; }); ArduinoOTA.onEnd([] { otaActive = false; }); ArduinoOTA.onError([](ota_error_t) { otaActive = false; });
#endif
#if S3_MDNS_ENABLED
  mdnsReady = MDNS.begin(hostName); if (mdnsReady) { MDNS.addService("http", "tcp", 80); MDNS.enableArduino(3232, true); }
#endif
#if S3_OTA_ENABLED
  ArduinoOTA.begin(); otaServiceRunning = true;
#endif
  Serial.printf("{\"type\":\"NETWORK\",\"ap\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"hostname\":\"%s\",\"mdns\":%s}\n",
                apReady ? "true" : "false", apSsid, WiFi.softAPIP().toString().c_str(), hostName, mdnsReady ? "true" : "false");
}

void tick() {
#if S3_HTTP_ENABLED
  server.handleClient();
#endif
#if (S3_WIFI_MODE == 1 || S3_WIFI_MODE == 3) && S3_STA_BEHAVIOR == 2
  tickSta();
#endif
#if S3_OTA_ENABLED
  const State current = state.load();
  const bool otaAllowed = current == State::IDLE || current == State::CLOSED || current == State::FAILED;
  if (!otaAllowed && otaServiceRunning) { ArduinoOTA.end(); if (mdnsReady) MDNS.disableArduino(); otaServiceRunning = false; otaActive = false; }
  else if (otaAllowed && !otaServiceRunning) { ArduinoOTA.begin(); if (mdnsReady) MDNS.enableArduino(3232, true); otaServiceRunning = true; }
  if (otaAllowed && otaServiceRunning) ArduinoOTA.handle();
#endif
}
}
