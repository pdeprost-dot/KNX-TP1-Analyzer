#include "hmi.h"
#include "analysis.h"
#include "acquisition.h"
#include "events.h"
#include "storage.h"
#include "tp1_decoder.h"
#include "wifi_manager.h"

#include <Arduino_GFX_Library.h>
#include <Wire.h>

namespace hmi {
namespace {
enum class Page : uint8_t { Dashboard, Scope, Events, System };
Arduino_GFX *screen = nullptr;
Page page = Page::Dashboard;
bool dirty = true;
uint32_t lastDrawMs = 0;
uint32_t lastTouchPollMs = 0;
uint32_t lastTapMs = 0;
uint32_t lastActiveTouchMs = 0;
bool touched = false;
uint32_t lastEventCount = 0;
constexpr uint16_t kWindowsMs[] = {5, 10, 20, 50, 100};
uint8_t windowIndex = 2;
bool autoScale = false;
uint32_t lastLiveFrameMs = 0;
uint32_t lastLiveStatsMs = 0;
uint32_t liveFrames = 0;
uint32_t liveSkipped = 0;
uint32_t liveTotalUs = 0;
uint32_t liveMaxUs = 0;
scope::Waveform liveWave = {};
uint16_t liveColumn = scope::kWaveColumns;
uint32_t liveFrameDrawUs = 0;
bool lastScopeRunning = false;
uint32_t lastScopeCapture = 0;
constexpr bool kLiveScopeDuringAnalysis = false;
constexpr uint16_t black = 0x0000;
constexpr uint16_t white = 0xFFFF;
constexpr uint16_t green = 0x07E0;
constexpr uint16_t cyan = 0x07FF;
constexpr uint16_t amber = 0xFFE0;
constexpr uint16_t red = 0xF800;

void text(int x, int y, const String &value, uint16_t color = white, uint8_t size = 1) {
  screen->setTextColor(color);
  screen->setTextSize(size);
  screen->setCursor(x, y);
  screen->print(value);
}

void navigation() {
  screen->drawFastHLine(0, 244, 172, 0x7BEF);
  const char *names[4] = {"DASH", "SCOPE", "EVENT", "SYS"};
  for (int i = 0; i < 4; ++i) text(i * 43 + 3, 252, names[i], static_cast<int>(page) == i ? cyan : white);
}

void button(int x, int y, int w, const char *label, uint16_t color) {
  screen->drawRect(x, y, w, 33, color);
  text(x + 8, y + 10, label, color);
}

void dashboard() {
  const auto a = analysis::status();
  const auto adc = scope::status();
  const auto wifi = network::status();
  text(6, 5, "KNX ANALYZER", cyan, 2);
  if (wifi.mode == network::Mode::AccessPoint && !wifi.primaryConfigured && !wifi.backupConfigured) {
    text(6, 40, "SETUP MODE", amber, 2);
    text(6, 83, "Wi-Fi:", white);
    text(6, 104, wifi.apSsid, cyan);
    text(6, 127, "192.168.4.1", white, 2);
    text(6, 164, "Connect then open", white);
    text(6, 181, "the address above", white);
    navigation();
    button(6, 280, 160, a.state == analysis::State::Running ? "STOP ANALYSIS" : "START ANALYSIS", a.state == analysis::State::Running ? red : green);
    return;
  }
  const auto sd = storage::status();
  text(6, 34, a.state == analysis::State::Running ? "ANALYZING" : analysis::name(a.state),
       a.state == analysis::State::Running ? green : amber, 2);
  text(6, 65, String("SCOPE ") + (adc.initialized ? "READY" : "ERROR"));
  String sessionId = a.state == analysis::State::Running ? sd.currentSession : sd.lastSession;
  if (sessionId.length() > 7) sessionId = sessionId.substring(sessionId.length() - 7);
  text(6, 82, String("SESSION ") + (sessionId.length() ? sessionId : "--"));
  text(6, 99, "KNX   NOT CONNECTED");
  const uint32_t visibleEvents = a.state == analysis::State::Running ? sd.currentEventCount : sd.lastEventCount;
  text(6, 124, String("EVENTS ") + String(visibleEvents) + "   ERRORS " + String(adc.readErrors + adc.overruns + sd.writeErrors));
  text(6, 141, String("ADC ") + String(adc.measuredHz / 1000.0f, 1) + " kS/s");
  text(6, 163, String("WIFI ") + network::modeName(wifi.mode));
  text(6, 180, wifi.ssid.substring(0, 25));
  text(6, 197, wifi.ip);
  if (wifi.mode == network::Mode::Station) text(6, 214, String(wifi.rssi) + " dBm");
  navigation();
  button(6, 280, 160, a.state == analysis::State::Running ? "STOP ANALYSIS" : "START ANALYSIS", a.state == analysis::State::Running ? red : green);
}

void scopeMetrics(uint16_t minRaw, uint16_t maxRaw, bool hasWave) {
  screen->fillRect(0, 193, 172, 10, black);
  text(6, 194, hasWave ? String("RAW MIN ") + minRaw + " MAX " + maxRaw : "ADC RAW", white);
}

void scopeFooter(uint16_t minRaw, uint16_t maxRaw, bool hasWave) {
  screen->fillRect(0, 193, 172, 50, black);
  scopeMetrics(minRaw, maxRaw, hasWave);
  screen->drawRect(4, 204, 80, 39, amber);
  screen->drawRect(88, 204, 80, 39, amber);
  text(10, 215, String(kWindowsMs[windowIndex]) + "ms", amber, 2);
  text(102, 215, autoScale ? "AUTO" : "FULL", amber, 2);
}

void scopeGraph(const scope::Waveform &wave, bool live) {
  constexpr int x0 = 6, y0 = 56, width = 160, height = 136;
  screen->fillRect(x0, y0, width, height, black);
  screen->drawRect(x0, y0, width, height, 0x7BEF);
  uint16_t lowScale = 0, highScale = 4095;
  if (autoScale) {
    const uint16_t span = wave.maxRaw > wave.minRaw ? wave.maxRaw - wave.minRaw : 1;
    const uint16_t margin = max<uint16_t>(16, span / 10);
    lowScale = wave.minRaw > margin ? wave.minRaw - margin : 0;
    highScale = min<uint32_t>(4095, static_cast<uint32_t>(wave.maxRaw) + margin);
    if (highScale <= lowScale) highScale = lowScale + 1;
  }
  const uint32_t scale = highScale - lowScale;
  for (int x = 0; x < scope::kWaveColumns; ++x) {
    const uint16_t lo = constrain(wave.low[x], lowScale, highScale);
    const uint16_t hi = constrain(wave.high[x], lowScale, highScale);
    const int top = y0 + height - 2 - (static_cast<uint32_t>(hi - lowScale) * (height - 3)) / scale;
    const int bottom = y0 + height - 2 - (static_cast<uint32_t>(lo - lowScale) * (height - 3)) / scale;
    screen->drawFastVLine(x0 + x, top, bottom - top + 1, cyan);
  }
  if (!live && wave.captured) screen->drawFastVLine(x0 + 112, y0 + 1, height - 2, red);
  scopeFooter(wave.minRaw, wave.maxRaw, true);
}

void liveGraphChunk() {
  constexpr int x0 = 6, y0 = 56, height = 136;
  uint16_t lowScale = 0, highScale = 4095;
  if (autoScale) {
    const uint16_t span = liveWave.maxRaw > liveWave.minRaw ? liveWave.maxRaw - liveWave.minRaw : 1;
    const uint16_t margin = max<uint16_t>(16, span / 10);
    lowScale = liveWave.minRaw > margin ? liveWave.minRaw - margin : 0;
    highScale = min<uint32_t>(4095, static_cast<uint32_t>(liveWave.maxRaw) + margin);
    if (highScale <= lowScale) highScale = lowScale + 1;
  }
  const uint32_t scale = highScale - lowScale;
  const uint16_t end = min<uint16_t>(scope::kWaveColumns, liveColumn + 16);
  for (uint16_t x = liveColumn; x < end; ++x) {
    const uint16_t lo = constrain(liveWave.low[x], lowScale, highScale);
    const uint16_t hi = constrain(liveWave.high[x], lowScale, highScale);
    const int top = y0 + height - 2 - (static_cast<uint32_t>(hi - lowScale) * (height - 3)) / scale;
    const int bottom = y0 + height - 2 - (static_cast<uint32_t>(lo - lowScale) * (height - 3)) / scale;
    screen->drawFastVLine(x0 + x, y0 + 1, height - 2, black);
    screen->drawFastVLine(x0 + x, top, bottom - top + 1, cyan);
  }
  liveColumn = end;
  if (liveColumn == scope::kWaveColumns) {
    scopeMetrics(liveWave.minRaw, liveWave.maxRaw, true);
  }
}

void tp1Monitor() {
  const auto decoded = tp1::status();
  const auto adc = scope::status();
  text(6, 5, "TP1 MONITOR", green, 2);
  text(6, 35, String(adc.measuredHz / 1000.0f, 1) + " kS/s GPIO5");
  text(6, 62, String("VALID ") + decoded.validUnknown, green, 2);
  text(6, 89, String("ACK ") + decoded.acks + " UNK " + decoded.undecoded, amber);
  text(6, 108, String("ERRORS ") + (decoded.parityErrors + decoded.timingErrors + decoded.invalidChecksum), red);
  const auto sd = storage::status();
  text(6, 124, String("ADC ") + adc.overruns + " DMA " + adc.readErrors + " SD " + sd.writeErrors);
  text(6, 138, String("PULSES ") + decoded.pulses + " BYTES " + decoded.characters);
  text(6, 151, String(decoded.lastRoute).substring(0, 26), cyan);
  text(6, 166, "LAST RAW", cyan);
  const String raw(decoded.lastHex);
  for (uint8_t line = 0; line < 3 && line * 24 < raw.length(); ++line)
    text(6, 184 + line * 15, raw.substring(line * 24, line * 24 + 24));
  text(6, 229, decoded.lastAck[0] ? String("PASSIVE ") + decoded.lastAck : "PASSIVE RX", green);
  navigation();
  button(6, 280, 77, "ARM", green);
  button(89, 280, 77, "MANUAL", amber);
}

void scopePage() {
  const auto adc = scope::status();
  const bool live = analysis::status().state == analysis::State::Running && adc.running;
  if (live) { tp1Monitor(); return; }
  text(6, 5, live ? "SCOPE LIVE" : "STOPPED", live ? green : amber, 2);
  uint32_t scopeHz = adc.measuredHz;
  events::Event latestEvent;
  if (adc.captured && events::newest(0, latestEvent) && latestEvent.captureNumber == adc.captureNumber)
    scopeHz = latestEvent.sampleRate;
  text(6, 32, String(scopeHz / 1000.0f, 1) + " kS/s GPIO5");
  scope::Waveform wave = {};
  if (!live && adc.captured && scope::waveform(wave)) {
    scopeGraph(wave, false);
  } else {
    screen->drawRect(6, 56, 160, 136, 0x7BEF);
    if (!live) text(16, 120, "ANALYSIS STOPPED", amber);
    scopeFooter(0, 0, false);
  }
  navigation();
  button(6, 280, 77, "ARM", green);
  button(89, 280, 77, "MANUAL", amber);
}

void placeholder(const char *title) {
  text(6, 5, "KNX ANALYZER", cyan, 2);
  text(6, 58, title, white, 2);
  text(6, 100, "Not implemented yet", amber);
  navigation();
  button(6, 280, 160, "BACK TO DASH", cyan);
}

void draw() {
  if (!screen) return;
  screen->fillScreen(black);
  switch (page) {
    case Page::Dashboard: dashboard(); break;
    case Page::Scope: scopePage(); break;
    case Page::Events: placeholder("EVENTS"); break;
    case Page::System: placeholder("SYSTEM"); break;
  }
  lastDrawMs = millis();
  dirty = false;
}

bool readTouch(uint16_t &x, uint16_t &y) {
  if (digitalRead(21) != LOW) return false;
  Wire.beginTransmission(0x63);
  Wire.write(0x01);
  if (Wire.endTransmission(true) != 0 || Wire.requestFrom(static_cast<uint8_t>(0x63), static_cast<uint8_t>(6)) != 6) return false;
  uint8_t data[6];
  for (uint8_t &byte : data) byte = Wire.read();
  if (data[1] != 1) return false;
  const uint16_t rawX = ((data[2] & 0x0F) << 8) | data[3];
  const uint16_t rawY = ((data[4] & 0x0F) << 8) | data[5];
  if ((rawX == 0 && rawY == 0) || rawX >= 172 || rawY >= 320) return false;
  x = 171 - rawX; // panel X direction is opposite to LCD portrait orientation
  y = 319 - rawY;
  return true;
}

void tap(uint16_t x, uint16_t y) {
  Serial.printf("{\"type\":\"TOUCH_TAP\",\"x\":%u,\"y\":%u}\n", x, y);
  if (y >= 244 && y < 276) {
    page = static_cast<Page>(min(3, x / 43));
  } else if (page == Page::Scope && y >= 204 && y < 244) {
    if (x < 86) windowIndex = (windowIndex + 1) % (sizeof(kWindowsMs) / sizeof(kWindowsMs[0]));
    else autoScale = !autoScale;
  } else if (y >= 280) {
    if (page == Page::Dashboard) {
      if (analysis::status().state == analysis::State::Running) analysis::stop();
      else analysis::start();
    } else if (page == Page::Scope) {
      if (x < 86) {
        scope::arm(scope::TriggerMode::Manual);
        if (analysis::status().state == analysis::State::Running) scope::start();
      } else {
        scope::manualTrigger();
      }
    } else page = Page::Dashboard;
  }
  dirty = true;
}
}

void begin(Arduino_GFX *display) { screen = display; dirty = true; }
void refresh() { dirty = true; }
void showScope() { page = Page::Scope; dirty = true; }

void tick() {
  if (!screen) return;
  const uint32_t now = millis();
  if (events::count() != lastEventCount) { lastEventCount = events::count(); dirty = true; }
  if (now - lastTouchPollMs >= 5) {
    lastTouchPollMs = now;
    uint16_t x = 0, y = 0;
    const bool active = readTouch(x, y);
    if (active && !touched) {
      touched = true;
      if (now - lastTapMs > 250) {
        lastTapMs = now;
        tap(x, y);
      }
    }
    if (active) lastActiveTouchMs = now;
    if (now - lastActiveTouchMs > 100) touched = false;
  }
  const auto adc = page == Page::Scope ? scope::status() : scope::Status{};
  const bool scopeRunning = page == Page::Scope && analysis::status().state == analysis::State::Running && adc.running;
  if (page == Page::Scope && (scopeRunning != lastScopeRunning || adc.captureNumber != lastScopeCapture)) dirty = true;
  lastScopeRunning = scopeRunning;
  lastScopeCapture = adc.captureNumber;
  if (dirty || (page == Page::Scope && scopeRunning && now - lastDrawMs >= 1000) ||
      (page != Page::Scope && now - lastDrawMs >= 5000)) draw();
  if (page == Page::Scope && scopeRunning && kLiveScopeDuringAnalysis) {
    if (liveColumn == scope::kWaveColumns && now - lastLiveFrameMs >= 125) {
      lastLiveFrameMs = now;
      if (storage::busy()) ++liveSkipped;
      else if (scope::liveWaveform(kWindowsMs[windowIndex], liveWave)) {
        liveColumn = 0;
        liveFrameDrawUs = 0;
      } else ++liveSkipped;
    }
    if (liveColumn < scope::kWaveColumns && !storage::busy()) {
      const uint32_t began = micros();
      liveGraphChunk();
      liveFrameDrawUs += micros() - began;
      if (liveColumn == scope::kWaveColumns) {
        liveTotalUs += liveFrameDrawUs;
        if (liveFrameDrawUs > liveMaxUs) liveMaxUs = liveFrameDrawUs;
        ++liveFrames;
      }
    }
  } else liveColumn = scope::kWaveColumns;
  if (now - lastLiveStatsMs >= 5000) {
    const uint32_t elapsed = now - lastLiveStatsMs;
    Serial.printf("{\"type\":\"LCD_LIVE_STATS\",\"fps\":%.1f,\"frame_avg_us\":%lu,\"frame_max_us\":%lu,\"skipped\":%lu,\"window_ms\":%u,\"scale\":\"%s\",\"heap_free\":%u}\n",
                  elapsed ? 1000.0f * liveFrames / elapsed : 0.0f,
                  liveFrames ? liveTotalUs / liveFrames : 0, liveMaxUs, liveSkipped,
                  kWindowsMs[windowIndex], autoScale ? "AUTO" : "FULL", ESP.getFreeHeap());
    lastLiveStatsMs = now;
    liveFrames = liveSkipped = liveTotalUs = liveMaxUs = 0;
  }
}
}
