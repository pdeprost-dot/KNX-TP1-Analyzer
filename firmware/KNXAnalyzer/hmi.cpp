#include "hmi.h"
#include "analysis.h"
#include "acquisition.h"
#include "events.h"
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
  text(6, 34, analysis::name(a.state), a.state == analysis::State::Running ? green : amber, 2);
  text(6, 65, String("SCOPE ") + (adc.initialized ? "READY" : "ERROR"));
  text(6, 82, "BUS   --.- V");
  text(6, 99, "KNX   NOT CONNECTED");
  text(6, 124, String("EVENTS ") + String(events::count()) + "   ERRORS " + String(adc.readErrors + adc.overruns));
  text(6, 141, String("ADC ") + String(adc.measuredHz / 1000.0f, 1) + " kS/s");
  text(6, 163, String("WIFI ") + network::modeName(wifi.mode));
  text(6, 180, wifi.ssid.substring(0, 25));
  text(6, 197, wifi.ip);
  if (wifi.mode == network::Mode::Station) text(6, 214, String(wifi.rssi) + " dBm");
  navigation();
  button(6, 280, 160, a.state == analysis::State::Running ? "STOP ANALYSIS" : "START ANALYSIS", a.state == analysis::State::Running ? red : green);
}

void scopePage() {
  const auto adc = scope::status();
  text(6, 5, adc.captured ? "CAPTURED" : (adc.running ? "SCOPE RUN" : "SCOPE HOLD"), adc.captured ? amber : green, 2);
  uint32_t scopeHz = adc.measuredHz;
  events::Event latestEvent;
  if (adc.captured && events::newest(0, latestEvent) && latestEvent.captureNumber == adc.captureNumber)
    scopeHz = latestEvent.sampleRate;
  text(6, 32, String(scopeHz / 1000.0f, 1) + " kS/s  GPIO5");
  scope::Waveform wave = {};
  constexpr int x0 = 6;
  constexpr int y0 = 58;
  constexpr int height = 172;
  screen->drawRect(x0, y0, 160, height, 0x7BEF);
  if (scope::waveform(wave)) {
    if (wave.captured) screen->drawFastVLine(x0 + 112, y0, height, red);
    for (int x = 0; x < scope::kWaveColumns; ++x) {
      int top = y0 + height - 1 - (static_cast<uint32_t>(wave.high[x]) * (height - 1)) / 4095;
      int bottom = y0 + height - 1 - (static_cast<uint32_t>(wave.low[x]) * (height - 1)) / 4095;
      screen->drawFastVLine(x0 + x, top, bottom - top + 1, cyan);
    }
    text(6, 232, String(wave.minRaw) + "/" + String(wave.maxRaw) + "  " + String(wave.samples));
  } else text(16, 130, "No samples yet", amber);
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

void tick() {
  if (!screen) return;
  const uint32_t now = millis();
  if (events::count() != lastEventCount) { lastEventCount = events::count(); dirty = true; }
  if (now - lastTouchPollMs >= 5) {
    lastTouchPollMs = now;
    uint16_t x = 0, y = 0;
    const bool active = readTouch(x, y);
    if (active && !touched && now - lastTapMs > 250) {
      lastTapMs = now;
      tap(x, y);
    }
    if (active) lastActiveTouchMs = now;
    if (now - lastActiveTouchMs > 100) touched = false;
  }
  const uint32_t period = page == Page::Scope ? 2000 : 5000;
  if (dirty || now - lastDrawMs >= period) draw();
}
}
