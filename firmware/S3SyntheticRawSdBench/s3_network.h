#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>

// Experimental STA + HTTP layer. It observes a bounded RAM snapshot only and
// never opens the SD card or changes the storage state.
namespace s3net {

struct Snapshot {
  char captureState[12] = {}, storageState[12] = {}, completionStatus[40] = {}, ip[16] = {};
  int32_t rssi = 0;
  uint64_t uptimeMs = 0, durationUs = 0, chunks = 0, samples = 0, rawBytes = 0, lostSamples = 0;
  uint32_t r1 = 0, rawFailures = 0, retryFailures = 0, reopenFailures = 0, sdErrors = 0, gaps = 0;
  uint32_t poolExhaustion = 0, freeMin = 0, pendingMax = 0, readyCurrent = 0, readyMax = 0;
  uint32_t writeUsMax = 0, checkpointUsMax = 0, writerHoldUsMax = 0;
  uint32_t heapInternalFree = 0, heapInternalMin = 0, heapDmaFree = 0, heapDmaMin = 0;
  bool invariant = false, pass = false, staConnected = false;
};

WebServer server(80);
Snapshot snapshot;
String staSsid, staPassword;
uint32_t httpRequests = 0, httpErrors = 0, httpResponseUsMax = 0, staReconnects = 0, nextReconnectMs = 0;
uint32_t networkStops = 0, networkStarts = 0;
bool networkActive = false, radioOffConfirmed = false;
constexpr uint32_t RECONNECT_INTERVAL_MS = 30000;

void refreshSnapshot() {
  const State current = state.load();
  const uint64_t now = esp_timer_get_time();
  const uint64_t end = (current == State::CLOSED || current == State::FAILED) && closedUs ? closedUs : now;
  snprintf(snapshot.captureState, sizeof(snapshot.captureState), "%s", stateName(current));
  snprintf(snapshot.storageState, sizeof(snapshot.storageState), "%s", storageStateName(storageState.load()));
  snprintf(snapshot.completionStatus, sizeof(snapshot.completionStatus), "%s", completionStatus);
  snapshot.uptimeMs = millis(); snapshot.durationUs = startedUs ? end - startedUs : 0;
  snapshot.chunks = producedChunks.load(); snapshot.samples = producedSamples.load(); snapshot.rawBytes = rawBytes.load();
  snapshot.lostSamples = lostSamples.load(); snapshot.r1 = r1Count.load(); snapshot.rawFailures = rawHandleFailures.load();
  snapshot.retryFailures = retryFailures.load(); snapshot.reopenFailures = reopenFailures.load(); snapshot.sdErrors = sdErrors.load();
  snapshot.gaps = gapCount.load(); snapshot.poolExhaustion = poolExhaustion.load(); snapshot.freeMin = freeMin;
  snapshot.pendingMax = pendingMax; snapshot.readyCurrent = readyQ ? uxQueueMessagesWaiting(readyQ) : 0; snapshot.readyMax = readyMax;
  snapshot.writeUsMax = writeLatencyMaxUs; snapshot.checkpointUsMax = checkpointLatencyMaxUs; snapshot.writerHoldUsMax = writerHoldMaxUs;
  snapshot.heapInternalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  snapshot.heapInternalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  snapshot.heapDmaFree = heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  snapshot.heapDmaMin = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  snapshot.invariant = invariantOk; snapshot.pass = finalPass; snapshot.staConnected = WiFi.status() == WL_CONNECTED;
  snapshot.rssi = snapshot.staConnected ? WiFi.RSSI() : 0;
  snprintf(snapshot.ip, sizeof(snapshot.ip), "%s", snapshot.staConnected ? WiFi.localIP().toString().c_str() : "");
}

void sendStatus() {
  const uint64_t started = esp_timer_get_time(); ++httpRequests;
  char json[1800];
  snprintf(json, sizeof(json),
    "{\"api_version\":\"1.0\",\"capture_state\":\"%s\",\"storage_state\":\"%s\","
    "\"completion_status\":\"%s\",\"uptime_ms\":\"%llu\",\"duration_us\":\"%llu\","
    "\"chunks\":\"%llu\",\"samples\":\"%llu\",\"raw_bytes\":\"%llu\","
    "\"R1\":%u,\"raw_handle_failures\":%u,\"retry_failures\":%u,\"reopen_failures\":%u,"
    "\"sd_errors\":%u,\"gaps\":%u,\"lost_samples\":\"%llu\",\"pool_exhaustion\":%u,"
    "\"free_min\":%u,\"pending_max\":%u,\"ready_current\":%u,\"ready_max\":%u,"
    "\"write_us_max\":%u,\"checkpoint_us_max\":%u,\"writer_hold_us_max\":%u,"
    "\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"heap_dma_free\":%u,\"heap_dma_min\":%u,"
    "\"sta_connected\":%s,\"sta_ip\":\"%s\",\"rssi\":%d,\"sta_reconnects\":%u,"
    "\"http_requests\":%u,\"http_errors\":%u,\"http_response_us_max\":%u,"
    "\"invariant\":%s,\"pass\":%s}",
    snapshot.captureState, snapshot.storageState, snapshot.completionStatus,
    snapshot.uptimeMs, snapshot.durationUs, snapshot.chunks, snapshot.samples, snapshot.rawBytes,
    snapshot.r1, snapshot.rawFailures, snapshot.retryFailures, snapshot.reopenFailures,
    snapshot.sdErrors, snapshot.gaps, snapshot.lostSamples, snapshot.poolExhaustion,
    snapshot.freeMin, snapshot.pendingMax, snapshot.readyCurrent, snapshot.readyMax,
    snapshot.writeUsMax, snapshot.checkpointUsMax, snapshot.writerHoldUsMax,
    snapshot.heapInternalFree, snapshot.heapInternalMin, snapshot.heapDmaFree, snapshot.heapDmaMin,
    snapshot.staConnected ? "true" : "false", snapshot.ip, snapshot.rssi, staReconnects,
    httpRequests, httpErrors, httpResponseUsMax, snapshot.invariant ? "true" : "false", snapshot.pass ? "true" : "false");
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
  const uint32_t elapsed = uint32_t(esp_timer_get_time() - started);
  if (elapsed > httpResponseUsMax) httpResponseUsMax = elapsed;
}

constexpr char mainPage[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><title>S3 Network/Web Bench</title><style>body{margin:auto;max-width:760px;padding:20px;background:#101820;color:#eef;font:16px system-ui}h1{color:#53d8fb}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:12px}.card{background:#1c2935;border-radius:12px;padding:14px}.label{color:#9fb3c8;font-size:.8rem;text-transform:uppercase}.value{font:600 1.2rem ui-monospace,monospace;margin-top:5px}</style></head><body><h1>S3 Network/Web Bench</h1><p id=link>Connecting...</p><div class=grid id=cards></div><script>const names=['capture_state','storage_state','duration_us','samples','chunks','raw_bytes','R1','gaps','lost_samples','pool_exhaustion','free_min','pending_max','ready_max','heap_internal_min','heap_dma_min','rssi'];let busy=false;async function poll(){if(busy)return;busy=true;try{const d=await fetch('/api/v1/status',{cache:'no-store'}).then(r=>r.json());link.textContent=d.sta_ip+' · HTTP '+d.http_requests;cards.innerHTML=names.map(k=>'<div class=card><div class=label>'+k+'</div><div class=value>'+d[k]+'</div></div>').join('')}catch(e){link.textContent='Disconnected'}finally{busy=false;setTimeout(poll,1000)}}poll()</script></body></html>)HTML";

void connectSta() {
  if (staSsid.isEmpty()) return;
  WiFi.begin(staSsid.c_str(), staPassword.c_str()); nextReconnectMs = millis() + RECONNECT_INTERVAL_MS;
}

void startNetwork() {
  if (networkActive) return;
  const bool modeReady = WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  connectSta();
  server.begin();
  networkActive = modeReady;
  radioOffConfirmed = false;
  ++networkStarts;
  Serial.printf("{\"type\":\"NETWORK_LIFECYCLE\",\"action\":\"START\",\"mode_result\":%s,\"effective_mode\":%u}\n",
                modeReady ? "true" : "false", unsigned(WiFi.getMode()));
}

bool stopForCapture() {
  if (state.load() == State::RUNNING || state.load() == State::STOPPING) return false;
  server.stop();
  WiFi.disconnect(true, false);
  const bool modeResult = WiFi.mode(WIFI_OFF);
  const uint32_t deadline = millis() + 1000;
  while (WiFi.getMode() != WIFI_OFF && int32_t(deadline - millis()) > 0) delay(1);
  radioOffConfirmed = modeResult && WiFi.getMode() == WIFI_OFF && WiFi.status() != WL_CONNECTED;
  networkActive = false;
  ++networkStops;
  Serial.printf("{\"type\":\"NETWORK_LIFECYCLE\",\"action\":\"STOP\",\"mode_result\":%s,"
                "\"effective_mode\":%u,\"sta_status\":%d,\"radio_off_confirmed\":%s}\n",
                modeResult ? "true" : "false", unsigned(WiFi.getMode()), int(WiFi.status()),
                radioOffConfirmed ? "true" : "false");
  return radioOffConfirmed;
}

void begin() {
  Preferences preferences; preferences.begin("s3-net", true);
  staSsid = preferences.getString("ssid1", ""); staPassword = preferences.getString("pass1", ""); preferences.end();
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(false); connectSta(); refreshSnapshot();
  server.on("/", HTTP_GET, [] { ++httpRequests; server.send_P(200, "text/html", mainPage); });
  server.on("/api/v1/status", HTTP_GET, sendStatus);
  server.onNotFound([] { ++httpRequests; ++httpErrors; server.send(404, "application/json", "{\"error\":\"not_found\"}"); });
  server.begin();
  networkActive = true; ++networkStarts;
  Serial.printf("{\"type\":\"NETWORK_WEB_INIT\",\"mode\":\"STA_ONLY\",\"ssid_configured\":%s,\"http_port\":80,\"poll_ms\":1000}\n",
                staSsid.isEmpty() ? "false" : "true");
}

void tick() {
  refreshSnapshot();
  const State current = state.load();
  if ((current == State::CLOSED || current == State::FAILED || current == State::IDLE) && !networkActive) startNetwork();
  if (!networkActive) return;
  server.handleClient();
  if (!staSsid.isEmpty() && WiFi.status() != WL_CONNECTED && int32_t(millis() - nextReconnectMs) >= 0) {
    ++staReconnects; WiFi.disconnect(false, false); connectSta();
  }
}

void printDiagnostics() {
  refreshSnapshot();
  Serial.printf("{\"type\":\"NETWORK_WEB_STATUS\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d,"
                "\"reconnects\":%u,\"http_requests\":%u,\"http_errors\":%u,\"http_response_us_max\":%u,"
                "\"network_active\":%s,\"wifi_mode\":%u,\"radio_off_confirmed\":%s,"
                "\"network_stops\":%u,\"network_starts\":%u}\n",
                snapshot.staConnected ? "true" : "false", snapshot.ip, snapshot.rssi,
                staReconnects, httpRequests, httpErrors, httpResponseUsMax,
                networkActive ? "true" : "false", unsigned(WiFi.getMode()), radioOffConfirmed ? "true" : "false",
                networkStops, networkStarts);
}

} // namespace s3net
