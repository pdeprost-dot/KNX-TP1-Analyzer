#pragma once

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Update.h>

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
  uint64_t events = 0, excursions = 0;
  uint32_t adcMin = 0, adcMax = 0, d44Max = 0, dmaOverflows = 0, adcErrors = 0;
  double adcMean = 0;
  uint64_t sdTotal = 0, sdUsed = 0;
  char staSsid[33] = {}, apSsid[33] = {}, apIp[16] = {};
  bool apActive = false;
};

WebServer server(80);
DNSServer dnsServer;
Snapshot snapshot;
struct StaProfile { String ssid, password; };
StaProfile staProfiles[2];
String apPassword;
String otaPassword;
char hostname[40] = {};
char apSsid[33] = {};
uint32_t httpRequests = 0, httpErrors = 0, httpResponseUsMax = 0, staReconnects = 0, nextReconnectMs = 0;
uint32_t networkStops = 0, networkStarts = 0;
bool networkActive = false, radioOffConfirmed = false;
bool mdnsActive = false, otaActive = false, otaConfigured = false;
bool captiveDnsActive = false;
bool startPending = false;
bool networkApplyPending = false;
bool webOtaAccepted = false, webOtaWriting = false, webOtaOk = false, rebootPending = false;
String webOtaError;
uint64_t pendingDurationUs = 0;
uint32_t startAfterMs = 0;
uint32_t networkApplyAfterMs = 0;
uint32_t rebootAfterMs = 0;
uint8_t staProfileIndex = 0;
bool staCyclePaused = false;
constexpr uint32_t RECONNECT_INTERVAL_MS = 30000;
constexpr uint32_t PROFILE_ATTEMPT_MS = 15000;
constexpr uint32_t NETWORK_CONFIG_VERSION = 2;

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
  snapshot.events = eventCount; snapshot.excursions = excursionCount;
  snapshot.adcMin = adcMin == UINT16_MAX ? 0 : adcMin; snapshot.adcMax = adcMax; snapshot.d44Max = d44Max;
  snapshot.dmaOverflows = dmaOverflows.load(); snapshot.adcErrors = adcReadErrors.load();
  snapshot.adcMean = producedSamples.load() ? double(adcSum) / producedSamples.load() : 0.0;
  snapshot.rssi = snapshot.staConnected ? WiFi.RSSI() : 0;
  snprintf(snapshot.ip, sizeof(snapshot.ip), "%s", snapshot.staConnected ? WiFi.localIP().toString().c_str() : "");
  snprintf(snapshot.staSsid, sizeof(snapshot.staSsid), "%s", snapshot.staConnected ? WiFi.SSID().c_str() : "");
  snapshot.apActive = WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA;
  snprintf(snapshot.apSsid, sizeof(snapshot.apSsid), "%s", apSsid);
  snprintf(snapshot.apIp, sizeof(snapshot.apIp), "%s", snapshot.apActive ? WiFi.softAPIP().toString().c_str() : "");
  if (current != State::RUNNING && current != State::STOPPING) {
    snapshot.sdTotal = SD.totalBytes(); snapshot.sdUsed = SD.usedBytes();
  }
}

void sendStatus() {
  const uint64_t started = esp_timer_get_time(); ++httpRequests;
  char json[2800];
  const String statusUnix = absoluteTimeValid ? String(startUnixMs) : "null";
  const String statusUtc = absoluteTimeValid ? "\"" + String(startUtc) + "\"" : "null";
  snprintf(json, sizeof(json),
    "{\"api_version\":\"1.0\",\"firmware_version\":\"%s\",\"capture_state\":\"%s\",\"storage_state\":\"%s\","
    "\"completion_status\":\"%s\",\"uptime_ms\":\"%llu\",\"duration_us\":\"%llu\","
    "\"chunks\":\"%llu\",\"samples\":\"%llu\",\"raw_bytes\":\"%llu\","
    "\"R1\":%u,\"raw_handle_failures\":%u,\"retry_failures\":%u,\"reopen_failures\":%u,"
    "\"sd_errors\":%u,\"gaps\":%u,\"lost_samples\":\"%llu\",\"pool_exhaustion\":%u,"
    "\"free_min\":%u,\"pending_max\":%u,\"ready_current\":%u,\"ready_max\":%u,"
    "\"write_us_max\":%u,\"checkpoint_us_max\":%u,\"writer_hold_us_max\":%u,"
    "\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"heap_dma_free\":%u,\"heap_dma_min\":%u,"
    "\"sta_connected\":%s,\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"rssi\":%d,\"sta_reconnects\":%u,"
    "\"ap_active\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"sd_total_bytes\":\"%llu\",\"sd_used_bytes\":\"%llu\","
    "\"http_requests\":%u,\"http_errors\":%u,\"http_response_us_max\":%u,"
    "\"pre_wifi_heap_ok\":%s,\"pre_wifi_internal_free\":%u,\"pre_wifi_internal_min\":%u,"
    "\"pre_wifi_internal_largest\":%u,\"pre_wifi_dma_free\":%u,\"pre_wifi_dma_min\":%u,"
    "\"events\":\"%llu\",\"excursions\":\"%llu\",\"adc_min\":%u,\"adc_max\":%u,"
    "\"adc_mean\":%.3f,\"d44_max\":%u,\"dma_overflow\":%u,\"adc_read_errors\":%u,"
    "\"pre_wifi_dma_largest\":%u,\"adc_task_stack_min_free\":%u,\"heap_integrity\":%s,"
    "\"invariant\":%s,\"pass\":%s,\"time_schema_version\":\"1.0\","
    "\"time_source\":\"%s\",\"absolute_time_valid\":%s,\"start_unix_ms\":%s,\"start_utc\":%s,"
    "\"monotonic_origin_us\":\"%llu\"}",
    FIRMWARE_VERSION, snapshot.captureState, snapshot.storageState, snapshot.completionStatus,
    snapshot.uptimeMs, snapshot.durationUs, snapshot.chunks, snapshot.samples, snapshot.rawBytes,
    snapshot.r1, snapshot.rawFailures, snapshot.retryFailures, snapshot.reopenFailures,
    snapshot.sdErrors, snapshot.gaps, snapshot.lostSamples, snapshot.poolExhaustion,
    snapshot.freeMin, snapshot.pendingMax, snapshot.readyCurrent, snapshot.readyMax,
    snapshot.writeUsMax, snapshot.checkpointUsMax, snapshot.writerHoldUsMax,
    snapshot.heapInternalFree, snapshot.heapInternalMin, snapshot.heapDmaFree, snapshot.heapDmaMin,
    snapshot.staConnected ? "true" : "false", snapshot.staSsid, snapshot.ip, snapshot.rssi, staReconnects,
    snapshot.apActive ? "true" : "false", snapshot.apSsid, snapshot.apIp, snapshot.sdTotal, snapshot.sdUsed,
    httpRequests, httpErrors, httpResponseUsMax, preWifiHeapOk ? "true" : "false",
    preWifiInternalFree, preWifiInternalMin, preWifiInternalLargest,
    preWifiDmaFree, preWifiDmaMin, snapshot.events, snapshot.excursions,
    snapshot.adcMin, snapshot.adcMax, snapshot.adcMean, snapshot.d44Max,
    snapshot.dmaOverflows, snapshot.adcErrors, preWifiDmaLargest,
    adcTaskStackMinFree.load() == UINT32_MAX ? 0 : adcTaskStackMinFree.load(),
    heap_caps_check_integrity_all(false) ? "true" : "false",
    snapshot.invariant ? "true" : "false", snapshot.pass ? "true" : "false",
    timeSourceName(activeTimeSource), absoluteTimeValid ? "true" : "false",
    statusUnix.c_str(), statusUtc.c_str(), startedUs);
  server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
  const uint32_t elapsed = uint32_t(esp_timer_get_time() - started);
  if (elapsed > httpResponseUsMax) httpResponseUsMax = elapsed;
}

constexpr char commonHead[] PROGMEM = R"HTML(<meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1"><style>body{margin:auto;max-width:900px;padding:18px;background:#101820;color:#eef;font:16px system-ui}nav{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px}a{color:#53d8fb}nav a{background:#1c2935;padding:10px 13px;border-radius:8px;text-decoration:none}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:10px}.card{background:#1c2935;border-radius:12px;padding:16px;margin:12px 0}.label{color:#9fb3c8;font-size:.78rem;text-transform:uppercase}.value{font:600 1.05rem ui-monospace,monospace;margin-top:4px;word-break:break-word}button,input,select{box-sizing:border-box;padding:11px;font:inherit;border-radius:7px;border:1px solid #607080;margin:4px 2px}button{background:#53d8fb;color:#07131b;font-weight:700;cursor:pointer}input,select{background:#101820;color:#fff;max-width:100%}.warn{border-left:4px solid #ffb454;padding-left:12px}.ok{color:#72e0a8}.bad{color:#ff7b72}pre{white-space:pre-wrap;word-break:break-word}.pw{display:flex;gap:4px;align-items:center;flex-wrap:wrap}.pw input{flex:1;min-width:180px}.scan{display:grid;grid-template-columns:1fr auto auto;gap:6px;align-items:center;padding:7px;border-bottom:1px solid #405060}</style>)HTML";
constexpr char nav[] PROGMEM = R"HTML(<nav><a href=/>Dashboard</a><a href=/network>Network</a><a href=/update>Update</a></nav>)HTML";
constexpr char mainPage[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><title>KNX Analyzer Field</title>%HEAD%</head><body>%NAV%<h1>KNX Analyzer Field</h1><p>Status is refreshed only on request. There is no automatic polling.</p><button onclick=load()>REFRESH STATUS</button><div class=grid id=cards></div><div class=card><h2>Last acquisition</h2><div id=last>Press REFRESH STATUS.</div></div><div class=card><h2>Start acquisition</h2><p class=warn>Wi-Fi, AP and all remote services will be disabled during acquisition. The Analyzer returns online only after storage finalization.</p><form id=start method=post action=/api/v1/start><label>Duration <select id=duration name=seconds><option value=60>1 min</option><option value=300>5 min</option><option value=600>10 min</option><option value=1800>30 min</option><option value=3600>1 h</option><option value=10800>3 h</option><option value=21600>6 h</option><option value=32400>9 h</option><option value=custom>Custom</option></select></label> <input id=custom name=custom_seconds type=number min=60 max=32400 placeholder="seconds" hidden> <input type=hidden id=start_unix_ms name=start_unix_ms><input type=hidden id=timezone_offset_min name=timezone_offset_min><input type=hidden name=time_source value=BROWSER><button>START</button></form></div><script>const fmt=n=>Number(n).toLocaleString();function card(k,v){return `<div class=card><div class=label>${k}</div><div class=value>${v??'-'}</div></div>`}async function load(){let d=await fetch('/api/v1/status',{cache:'no-store'}).then(r=>r.json());let free=Number(d.sd_total_bytes)-Number(d.sd_used_bytes);cards.innerHTML=[['Firmware',d.firmware_version],['Time',d.absolute_time_valid?new Date(Number(d.start_unix_ms)).toLocaleString()+'<br>'+d.time_source:'UNSYNCED<br>NONE'],['Analyzer',d.capture_state],['Completion',d.completion_status],['ADC',d.dma_overflow==0?'READY':'ERROR'],['Sample rate target','83,333 Hz'],['Last measured rate',d.duration_us>0?Math.round(Number(d.samples)/(Number(d.duration_us)/1e6)).toLocaleString()+' Hz':'-'],['Storage',d.storage_state],['SD free / total',fmt(free)+' / '+fmt(d.sd_total_bytes)],['STA',d.sta_connected?d.sta_ssid:'DISCONNECTED'],['IP',d.sta_ip],['RSSI',d.rssi+' dBm'],['AP',d.ap_active?d.ap_ssid+' '+d.ap_ip:'OFF'],['Uptime',Math.floor(Number(d.uptime_ms)/1000)+' s']].map(x=>card(x[0],x[1])).join('');let timing=d.absolute_time_valid?'Started: '+new Date(Number(d.start_unix_ms)).toLocaleString()+'<br>Ended: '+new Date(Number(d.start_unix_ms)+Number(d.duration_us)/1000).toLocaleString()+'<br>':'Started: UNSYNCED<br>';last.innerHTML=timing+`Duration: ${(+d.duration_us/1e6).toFixed(3)} s<br>Samples: ${fmt(d.samples)}<br>Events: ${fmt(d.events)}<br>Event RAW: ${fmt(d.raw_bytes)} bytes<br>Loss / gaps: ${d.lost_samples} / ${d.gaps}<br>EIO / R1 / R3: ${d.raw_handle_failures} / ${d.R1} / ${d.retry_failures}<br>Result: <b>${d.completion_status}</b>`}duration.onchange=()=>custom.hidden=duration.value!='custom';start.onsubmit=e=>{let s=duration.value=='custom'?+custom.value:+duration.value;if(!s||s<60||s>32400){e.preventDefault();alert('Duration must be 60..32400 seconds');return}let end=new Date(Date.now()+s*1000);if(!confirm(`Start ${s} seconds?\nApproximate end: ${end.toLocaleString()}\nWi-Fi will be switched off and the Analyzer will be unreachable until finalization.`)){e.preventDefault();return}start_unix_ms.value=Date.now();timezone_offset_min.value=new Date().getTimezoneOffset()};load()</script></body></html>)HTML";
constexpr char sessionsPage[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><title>Sessions</title>%HEAD%</head><body>%NAV%<h1>Sessions</h1><div class=card id=result>Loading last session...</div><p>This V2 exposes the current/last in-memory session only. No SD-wide session index is built.</p><script>fetch('/api/v1/status',{cache:'no-store'}).then(r=>r.json()).then(d=>result.innerHTML=`<h2>Last session</h2><p>State: <b>${d.capture_state} / ${d.completion_status}</b></p><p>Duration: ${(+d.duration_us/1e6).toFixed(3)} s<br>Events: ${d.events}<br>Samples: ${d.samples}<br>Data: ${d.raw_bytes} bytes<br>Loss / gaps: ${d.lost_samples} / ${d.gaps}<br>EIO / R1 / R3: ${d.raw_handle_failures} / ${d.R1} / ${d.retry_failures}</p>`)</script></body></html>)HTML";
constexpr char networkPage[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><title>Network</title>%HEAD%</head><body>%NAV%<h1>Network</h1><div class=card><pre id=status>Loading...</pre></div><div class=card><h2>Available Wi-Fi</h2><button type=button onclick=scan()>SCAN WI-FI</button><div id=scanResult>No scan requested.</div></div><form class=card id=nf method=post action=/api/v1/network><h2>Field Wi-Fi</h2><label>SSID <input name=field_ssid id=fs maxlength=32></label><div class=pw><input name=field_password id=fp type=password placeholder="Password (blank = unchanged)"><input name=field_password_confirm id=fpc type=password placeholder="Confirm password"><button type=button onclick=toggle('fp','fpc',this)>SHOW</button></div><label><input name=clear_field type=checkbox value=1> Clear profile</label><h2>Office Wi-Fi</h2><label>SSID <input name=office_ssid id=os maxlength=32></label><div class=pw><input name=office_password id=op type=password placeholder="Password (blank = unchanged)"><input name=office_password_confirm id=opc type=password placeholder="Confirm password"><button type=button onclick=toggle('op','opc',this)>SHOW</button></div><label><input name=clear_office type=checkbox value=1> Clear profile</label><h2>Analyzer AP</h2><div class=pw><input name=ap_password id=ap type=password minlength=8 placeholder="Password (blank = unchanged)"><input name=ap_password_confirm id=apc type=password minlength=8 placeholder="Confirm password"><button type=button onclick=toggle('ap','apc',this)>SHOW</button></div><button>SAVE AND APPLY</button></form><p>Connection order: Field first, then Office; each profile gets 15 seconds. After both fail, retry starts after 30 seconds. AP remains available outside acquisition. Hidden SSIDs can be entered manually.</p><script>function toggle(a,b,x){let show=document.getElementById(a).type=='password';document.getElementById(a).type=document.getElementById(b).type=show?'text':'password';x.textContent=show?'HIDE':'SHOW'}function pick(s,target){document.getElementById(target).value=s}async function scan(){scanResult.textContent='Scanning...';try{let a=await fetch('/api/v1/wifi-scan',{cache:'no-store'}).then(r=>r.json());scanResult.replaceChildren();if(!a.networks.length){scanResult.textContent='No network found.';return}for(const n of a.networks){let row=document.createElement('div'),name=document.createElement('span'),info=document.createElement('span'),actions=document.createElement('span'),bf=document.createElement('button'),bo=document.createElement('button');row.className='scan';name.textContent=n.ssid||'(hidden)';info.textContent=n.rssi+' dBm · '+n.security;bf.type=bo.type='button';bf.textContent='FIELD';bo.textContent='OFFICE';bf.onclick=()=>pick(n.ssid,'fs');bo.onclick=()=>pick(n.ssid,'os');actions.append(bf,bo);row.append(name,info,actions);scanResult.append(row)}}catch(e){scanResult.textContent='Scan failed — try again'}}nf.onsubmit=e=>{for(let p of [['fp','fpc'],['op','opc'],['ap','apc']])if(document.getElementById(p[0]).value!==document.getElementById(p[1]).value){e.preventDefault();alert('Password confirmation does not match');return}};fetch('/api/v1/network',{cache:'no-store'}).then(r=>r.json()).then(d=>{status.textContent=`STA: ${d.sta_connected?'CONNECTED':'DISCONNECTED'} ${d.sta_ssid} ${d.sta_ip}\nAP: ${d.ap_active?'ON':'OFF'} ${d.ap_ssid} ${d.ap_ip}\nField password: ${d.field_password_set?'configured':'not set'}\nOffice password: ${d.office_password_set?'configured':'not set'}`;fs.value=d.field_ssid;os.value=d.office_ssid})</script></body></html>)HTML";
constexpr char updatePage[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><title>Firmware Update</title>%HEAD%</head><body>%NAV%<h1>Firmware Update</h1><div class=card><p>Current version: <b>%VERSION%</b></p><p>Updates are accepted only in IDLE or CLOSED. Select an ESP32 application <code>.bin</code>; it is written to the inactive OTA partition and activated only after the official Update API validates the complete image.</p><form method=post action=/api/v1/update enctype=multipart/form-data onsubmit="return confirm('Install this firmware and reboot?')"><input type=file name=firmware accept=.bin,application/octet-stream required><button>UPDATE FIRMWARE</button></form></div></body></html>)HTML";

String renderPage(const char *source) {
  String page(source); page.replace("%HEAD%", FPSTR(commonHead)); page.replace("%NAV%", FPSTR(nav));
  page.replace("%VERSION%", FIRMWARE_VERSION); return page;
}

String renderMessage(const String &title, const String &body) {
  String page = "<!doctype html><html lang=en><head><title>" + title + "</title>" +
    String(FPSTR(commonHead)) + "</head><body>" + String(FPSTR(nav)) + "<h1>" + title +
    "</h1><div class=card>" + body + "</div></body></html>";
  return page;
}

String jsonEscape(const String &value) {
  String escaped; escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (c == '\\' || c == '"') { escaped += '\\'; escaped += c; }
    else if (c == '\n') escaped += "\\n";
    else if (uint8_t(c) >= 0x20) escaped += c;
  }
  return escaped;
}

const char *securityName(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "SECURED";
  }
}

void redirectToPortal() {
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "Open KNX Analyzer");
}

void startSafeServices() {
  if (WiFi.status() == WL_CONNECTED && !mdnsActive && MDNS.begin(hostname)) {
    MDNS.addService("http", "tcp", 80); mdnsActive = true;
  }
  if (!otaPassword.isEmpty() && !otaConfigured) {
    ArduinoOTA.setHostname(hostname); ArduinoOTA.setPassword(otaPassword.c_str());
    ArduinoOTA.setMdnsEnabled(false);
    ArduinoOTA.onStart([] { otaActive = true; Serial.println("{\"type\":\"OTA_START\"}"); });
    ArduinoOTA.onEnd([] { otaActive = false; Serial.println("{\"type\":\"OTA_END\",\"validation\":\"OK\"}"); });
    ArduinoOTA.onError([](ota_error_t error) { otaActive = false; Serial.printf("{\"type\":\"OTA_ERROR\",\"code\":%u}\n", unsigned(error)); });
    ArduinoOTA.begin(); otaConfigured = true;
  }
}

void connectSta(uint8_t index) {
  staProfileIndex = index > 1 ? 0 : index;
  if (staProfiles[staProfileIndex].ssid.isEmpty()) {
    const uint8_t alternate = staProfileIndex ^ 1;
    if (staProfiles[alternate].ssid.isEmpty()) { nextReconnectMs = millis() + RECONNECT_INTERVAL_MS; return; }
    staProfileIndex = alternate;
  }
  WiFi.begin(staProfiles[staProfileIndex].ssid.c_str(), staProfiles[staProfileIndex].password.c_str());
  nextReconnectMs = millis() + PROFILE_ATTEMPT_MS;
}

void loadNetworkConfig() {
  Preferences preferences; preferences.begin("s3-net", false);
  const uint32_t version = preferences.getUInt("cfg-ver", 0);
  if (version != NETWORK_CONFIG_VERSION) {
    const String legacySsid = preferences.getString("ssid1", "");
    const String legacyPassword = preferences.getString("pass1", "");
    const String legacyOta = preferences.getString("ota-pass", "");
    preferences.clear(); preferences.putUInt("cfg-ver", NETWORK_CONFIG_VERSION);
    if (!legacySsid.isEmpty()) { preferences.putString("field-ssid", legacySsid); preferences.putString("field-pass", legacyPassword); }
    if (!legacyOta.isEmpty()) preferences.putString("ota-pass", legacyOta);
  }
  staProfiles[0].ssid = preferences.getString("field-ssid", "");
  staProfiles[0].password = preferences.getString("field-pass", "");
  staProfiles[1].ssid = preferences.getString("office-ssid", "");
  staProfiles[1].password = preferences.getString("office-pass", "");
  otaPassword = preferences.getString("ota-pass", "");
  apPassword = preferences.getString("ap-pass", "");
  if (apPassword.length() < 8) {
    char generated[20]; snprintf(generated, sizeof(generated), "KNX-%08lx", (unsigned long)(ESP.getEfuseMac() & 0xffffffff));
    apPassword = generated; preferences.putString("ap-pass", apPassword);
  }
  preferences.end();
}

bool configureOtaPassword(const String &password) {
  if (password.length() < 12 || state.load() == State::RUNNING || state.load() == State::STOPPING) return false;
  Preferences preferences; preferences.begin("s3-net", false); preferences.putString("ota-pass", password); preferences.end();
  otaPassword = password;
  if (networkActive) { ArduinoOTA.end(); otaConfigured = otaActive = false; startSafeServices(); }
  return otaConfigured;
}

void startNetwork() {
  if (networkActive) return;
  const bool modeReady = WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(false);
  WiFi.softAP(apSsid, apPassword.c_str());
  captiveDnsActive = dnsServer.start(53, "*", WiFi.softAPIP());
  connectSta(0);
  server.begin();
  startSafeServices();
  networkActive = modeReady;
  radioOffConfirmed = false;
  ++networkStarts;
  Serial.printf("{\"type\":\"NETWORK_LIFECYCLE\",\"action\":\"START\",\"mode_result\":%s,\"effective_mode\":%u}\n",
                modeReady ? "true" : "false", unsigned(WiFi.getMode()));
}

bool stopForCapture() {
  if (state.load() == State::RUNNING || state.load() == State::STOPPING) return false;
  server.stop();
  if (captiveDnsActive) { dnsServer.stop(); captiveDnsActive = false; }
  ArduinoOTA.end(); otaConfigured = otaActive = false;
  if (mdnsActive) { MDNS.end(); mdnsActive = false; }
  WiFi.softAPdisconnect(true);
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
  const uint64_t chip = ESP.getEfuseMac();
  snprintf(hostname, sizeof(hostname), "knx-analyzer-%04x", unsigned(chip & 0xffff));
  snprintf(apSsid, sizeof(apSsid), "KNX-Analyzer-%04X", unsigned(chip & 0xffff));
  loadNetworkConfig();
  WiFi.mode(WIFI_AP_STA); WiFi.setAutoReconnect(false); WiFi.softAP(apSsid, apPassword.c_str());
  captiveDnsActive = dnsServer.start(53, "*", WiFi.softAPIP()); connectSta(0); refreshSnapshot();
  server.on("/", HTTP_GET, [] { ++httpRequests; server.send(200, "text/html", renderPage(mainPage)); });
  server.on("/sessions", HTTP_GET, [] { ++httpRequests; server.send(200, "text/html", renderPage(sessionsPage)); });
  server.on("/network", HTTP_GET, [] { ++httpRequests; server.send(200, "text/html", renderPage(networkPage)); });
  server.on("/update", HTTP_GET, [] { ++httpRequests; server.send(200, "text/html", renderPage(updatePage)); });
  server.on("/api/v1/status", HTTP_GET, sendStatus);
  server.on("/api/v1/wifi-scan", HTTP_GET, [] {
    ++httpRequests;
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) {
      server.send(409, "application/json", "{\"error\":\"acquisition_active\"}"); return;
    }
    const int count = WiFi.scanNetworks(false, true);
    if (count < 0) { server.send(500, "application/json", "{\"error\":\"scan_failed\"}"); return; }
    String json = "{\"networks\":[";
    for (int i = 0; i < count; ++i) {
      if (i) json += ',';
      json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) +
        ",\"security\":\"" + securityName(WiFi.encryptionType(i)) + "\"}";
    }
    json += "]}"; WiFi.scanDelete();
    server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
  });
  server.on("/api/v1/network", HTTP_GET, [] {
    ++httpRequests; refreshSnapshot();
    String json = "{\"sta_connected\":" + String(snapshot.staConnected ? "true" : "false") +
      ",\"sta_ssid\":\"" + String(snapshot.staSsid) + "\",\"sta_ip\":\"" + String(snapshot.ip) +
      "\",\"ap_active\":" + String(snapshot.apActive ? "true" : "false") + ",\"ap_ssid\":\"" + String(apSsid) +
      "\",\"ap_ip\":\"" + String(snapshot.apIp) + "\",\"field_ssid\":\"" + staProfiles[0].ssid +
      "\",\"office_ssid\":\"" + staProfiles[1].ssid + "\",\"field_password_set\":" +
      String(staProfiles[0].password.isEmpty() ? "false" : "true") + ",\"office_password_set\":" +
      String(staProfiles[1].password.isEmpty() ? "false" : "true") + "}";
    server.sendHeader("Cache-Control", "no-store"); server.send(200, "application/json", json);
  });
  server.on("/api/v1/network", HTTP_POST, [] {
    ++httpRequests;
    if (state.load() == State::RUNNING || state.load() == State::STOPPING) { server.send(409, "application/json", "{\"saved\":false,\"error\":\"acquisition_active\"}"); return; }
    if (server.arg("field_password") != server.arg("field_password_confirm") ||
        server.arg("office_password") != server.arg("office_password_confirm") ||
        server.arg("ap_password") != server.arg("ap_password_confirm")) {
      server.send(400, "text/html", renderMessage("Network configuration not saved",
        "<p class=bad>Password confirmation does not match.</p><p><a href=/network>RETURN TO NETWORK</a></p>")); return;
    }
    Preferences preferences; preferences.begin("s3-net", false); preferences.putUInt("cfg-ver", NETWORK_CONFIG_VERSION);
    for (uint8_t index = 0; index < 2; ++index) {
      const char *prefix = index ? "office" : "field";
      const String clearName = String("clear_") + prefix;
      const String ssidName = String(prefix) + "_ssid";
      const String passwordName = String(prefix) + "_password";
      const char *ssidKey = index ? "office-ssid" : "field-ssid";
      const char *passwordKey = index ? "office-pass" : "field-pass";
      if (server.hasArg(clearName)) { preferences.remove(ssidKey); preferences.remove(passwordKey); staProfiles[index] = StaProfile{}; }
      else {
        const String ssid = server.arg(ssidName); const String password = server.arg(passwordName);
        staProfiles[index].ssid = ssid; preferences.putString(ssidKey, ssid);
        if (ssid.isEmpty()) { staProfiles[index].password = ""; preferences.remove(passwordKey); }
        else if (!password.isEmpty()) { staProfiles[index].password = password; preferences.putString(passwordKey, password); }
      }
    }
    const String newApPassword = server.arg("ap_password");
    if (!newApPassword.isEmpty()) {
      if (newApPassword.length() < 8) { preferences.end(); server.send(400, "text/html", renderMessage("Network configuration not saved",
        "<p class=bad>The Analyzer AP password must contain at least 8 characters.</p><p><a href=/network>RETURN TO NETWORK</a></p>")); return; }
      apPassword = newApPassword; preferences.putString("ap-pass", apPassword);
    }
    preferences.end(); networkApplyPending = true; networkApplyAfterMs = millis() + 500;
    server.send(200, "text/html", renderMessage("Network configuration saved",
      "<p class=ok>Network configuration saved.</p><p>The Analyzer is restarting its network interfaces.</p>"
      "<p>Reconnect using the new network settings if necessary.</p><p><a href=/network>RETURN TO NETWORK</a></p>"));
  });
  server.on("/generate_204", HTTP_ANY, redirectToPortal);
  server.on("/gen_204", HTTP_ANY, redirectToPortal);
  server.on("/hotspot-detect.html", HTTP_ANY, redirectToPortal);
  server.on("/library/test/success.html", HTTP_ANY, redirectToPortal);
  server.on("/connecttest.txt", HTTP_ANY, redirectToPortal);
  server.on("/ncsi.txt", HTTP_ANY, redirectToPortal);
  server.on("/redirect", HTTP_ANY, redirectToPortal);
  server.on("/canonical.html", HTTP_ANY, redirectToPortal);
  server.on("/success.txt", HTTP_ANY, redirectToPortal);
  server.on("/fwlink", HTTP_ANY, redirectToPortal);
  server.on("/api/v1/start", HTTP_POST, [] {
    ++httpRequests;
    if (state.load() != State::IDLE && state.load() != State::CLOSED) { server.send(409, "application/json", "{\"accepted\":false,\"error\":\"state\"}"); return; }
    uint32_t seconds = server.hasArg("seconds") ? strtoul(server.arg("seconds").c_str(), nullptr, 10) : 60;
    if (server.arg("seconds") == "custom") seconds = strtoul(server.arg("custom_seconds").c_str(), nullptr, 10);
    if (seconds < 60 || seconds > 32400) { server.send(400, "application/json", "{\"accepted\":false,\"error\":\"duration_60_to_32400\"}"); return; }
    if (server.arg("time_source") == "BROWSER" && server.hasArg("start_unix_ms")) {
      const uint64_t unixMs = strtoull(server.arg("start_unix_ms").c_str(), nullptr, 10);
      const int32_t timezoneOffset = strtol(server.arg("timezone_offset_min").c_str(), nullptr, 10);
      prepareTimeBrowser(unixMs, timezoneOffset);
    } else prepareTimeNone();
    pendingDurationUs = uint64_t(seconds) * 1000000ULL; startPending = true; startAfterMs = millis() + 350;
    const String body = "<p class=ok><b>ACQUISITION IN PROGRESS</b></p><p>Duration: " + String(seconds) +
      " seconds</p><p>Started: <span id=started></span><br>Estimated end: <span id=ending></span></p>"
      "<div class=label>Time remaining</div><div class=value id=remaining>--:--:--</div>"
      "<p>The Analyzer is operating autonomously. Wi-Fi is disabled during acquisition.</p>"
      "<p id=done hidden>Acquisition should now be complete. The Analyzer may need a few seconds to finalize storage and restore Wi-Fi.</p>"
      "<button id=check disabled onclick=checkResult()>CHECK RESULT</button><p class=bad id=reach></p>"
      "<script>const start=Date.now(),deadline=start+" + String(seconds) +
      "*1000;started.textContent=new Date(start).toLocaleString();ending.textContent=new Date(deadline).toLocaleString();"
      "function draw(){let left=Math.max(0,deadline-Date.now()),s=Math.ceil(left/1000),h=Math.floor(s/3600),m=Math.floor(s%3600/60),q=s%60;remaining.textContent=[h,m,q].map(x=>String(x).padStart(2,'0')).join(':');if(!left){clearInterval(timer);done.hidden=false;check.disabled=false}}"
      "async function checkResult(){reach.textContent='';try{let r=await fetch('/api/v1/status',{cache:'no-store'});if(!r.ok)throw 0;location.href='/'}catch(e){reach.textContent='Analyzer not reachable yet — try again';check.textContent='TRY AGAIN'}}let timer=setInterval(draw,1000);draw()</script>";
    server.send(202, "text/html", renderMessage("Acquisition", body));
  });
  server.on("/api/v1/ota-password", HTTP_POST, [] {
    ++httpRequests;
    const String password = server.arg("password");
    if (!configureOtaPassword(password)) { server.send(400, "application/json", "{\"saved\":false}"); return; }
    server.send(200, "application/json", "{\"saved\":true,\"ota\":true}");
  });
  server.on("/api/v1/update", HTTP_POST, [] {
    ++httpRequests;
    if (!webOtaAccepted || !webOtaOk || Update.hasError()) {
      server.send(400, "text/html", renderMessage("Firmware update failed",
        "<p class=bad>" + webOtaError + "</p><p><a href=/update>RETURN TO UPDATE</a></p>")); return;
    }
    server.send(200, "text/html", renderMessage("Firmware update complete",
      "<p class=ok>Image validated.</p><p>The Analyzer will reboot now.</p>"));
    rebootPending = true; rebootAfterMs = millis() + 750;
  }, [] {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      const State current = state.load(); webOtaAccepted = current == State::IDLE || current == State::CLOSED;
      webOtaWriting = webOtaOk = false; webOtaError = "";
      if (!webOtaAccepted) { webOtaError = "Analyzer state is not safe for update."; return; }
      if (!upload.filename.endsWith(".bin")) { webOtaAccepted = false; webOtaError = "Only .bin application images are accepted."; return; }
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { webOtaAccepted = false; webOtaError = Update.errorString(); return; }
      webOtaWriting = true;
    } else if (upload.status == UPLOAD_FILE_WRITE && webOtaWriting) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { webOtaError = Update.errorString(); webOtaWriting = false; Update.abort(); }
    } else if (upload.status == UPLOAD_FILE_END && webOtaWriting) {
      webOtaOk = Update.end(true); webOtaWriting = false; if (!webOtaOk) webOtaError = Update.errorString();
    } else if (upload.status == UPLOAD_FILE_ABORTED) { Update.abort(); webOtaWriting = false; webOtaError = "Upload aborted."; }
  });
  server.onNotFound([] {
    ++httpRequests;
    if (WiFi.getMode() == WIFI_AP || WiFi.getMode() == WIFI_AP_STA) { redirectToPortal(); return; }
    ++httpErrors; server.send(404, "application/json", "{\"error\":\"not_found\"}");
  });
  server.begin();
  startSafeServices();
  networkActive = true; ++networkStarts;
  Serial.printf("{\"type\":\"NETWORK_WEB_INIT\",\"mode\":\"AP_STA\",\"ssid_configured\":%s,\"http_port\":80,\"polling\":false,\"hostname\":\"%s\",\"ap_ssid\":\"%s\",\"mdns\":%s,\"ota\":%s}\n",
                staProfiles[0].ssid.isEmpty() && staProfiles[1].ssid.isEmpty() ? "false" : "true", hostname, apSsid,
                mdnsActive ? "true" : "false", otaConfigured ? "true" : "false");
}

void tick() {
  refreshSnapshot();
  const State current = state.load();
  if ((current == State::CLOSED || current == State::FAILED || current == State::IDLE) && !networkActive) startNetwork();
  if (!networkActive) return;
  if (captiveDnsActive) dnsServer.processNextRequest();
  server.handleClient();
  if (WiFi.status() == WL_CONNECTED && !mdnsActive) startSafeServices();
  if (otaConfigured && (current == State::IDLE || current == State::CLOSED)) ArduinoOTA.handle();
  if (rebootPending && int32_t(millis() - rebootAfterMs) >= 0) { delay(50); ESP.restart(); }
  if (networkApplyPending && int32_t(millis() - networkApplyAfterMs) >= 0) {
    networkApplyPending = false;
    if (mdnsActive) { MDNS.end(); mdnsActive = false; }
    if (captiveDnsActive) { dnsServer.stop(); captiveDnsActive = false; }
    ArduinoOTA.end(); otaConfigured = otaActive = false;
    WiFi.disconnect(false, false); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apSsid, apPassword.c_str()); captiveDnsActive = dnsServer.start(53, "*", WiFi.softAPIP());
    connectSta(0); startSafeServices();
  }
  if (startPending && int32_t(millis() - startAfterMs) >= 0) {
    startPending = false; requestedDurationUs = pendingDurationUs;
    if (!stopForCapture() || !startRun()) Serial.println("{\"type\":\"WEB_START\",\"accepted\":false}");
  }
  if (WiFi.status() != WL_CONNECTED && int32_t(millis() - nextReconnectMs) >= 0) {
    ++staReconnects; WiFi.disconnect(false, false);
    if (staCyclePaused) { staCyclePaused = false; connectSta(0); }
    else if (staProfileIndex == 0 && !staProfiles[1].ssid.isEmpty()) connectSta(1);
    else { staCyclePaused = true; staProfileIndex = 0; nextReconnectMs = millis() + RECONNECT_INTERVAL_MS; }
  }
}

void printDiagnostics() {
  refreshSnapshot();
  Serial.printf("{\"type\":\"NETWORK_WEB_STATUS\",\"connected\":%s,\"ip\":\"%s\",\"rssi\":%d,"
                "\"reconnects\":%u,\"http_requests\":%u,\"http_errors\":%u,\"http_response_us_max\":%u,"
                "\"network_active\":%s,\"wifi_mode\":%u,\"radio_off_confirmed\":%s,"
                "\"network_stops\":%u,\"network_starts\":%u,\"hostname\":\"%s\",\"mdns\":%s,\"ota\":%s}\n",
                snapshot.staConnected ? "true" : "false", snapshot.ip, snapshot.rssi,
                staReconnects, httpRequests, httpErrors, httpResponseUsMax,
                networkActive ? "true" : "false", unsigned(WiFi.getMode()), radioOffConfirmed ? "true" : "false",
                networkStops, networkStarts, hostname, mdnsActive ? "true" : "false", otaConfigured ? "true" : "false");
}

} // namespace s3net
