#include "wifi_manager.h"

#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>

namespace network {
namespace {
struct Profile { String ssid; String password; } profiles[2];
Preferences prefs;
DNSServer dns;
Mode current = Mode::AccessPoint;
String apSsid;
bool apActive = false;
bool dnsActive = false;
bool connectPending = false;
uint32_t attemptStartedMs = 0;
uint32_t disconnectedSinceMs = 0;
constexpr uint32_t kConnectTimeoutMs = 12000;

void startAP() {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP);
  if (!apActive) {
    apActive = WiFi.softAP(apSsid.c_str());
    if (apActive) {
      dns.start(53, "*", WiFi.softAPIP());
      dnsActive = true;
    }
  }
  current = Mode::AccessPoint;
  Serial.printf("{\"type\":\"WIFI_STATUS\",\"mode\":\"AP\",\"ssid\":\"%s\",\"ip\":\"%s\"}\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void tryProfile(uint8_t slot) {
  if (profiles[slot].ssid.isEmpty()) {
    if (slot == 0 && !profiles[1].ssid.isEmpty()) {
      tryProfile(1);
    } else {
      startAP();
    }
    return;
  }
  current = slot == 0 ? Mode::ConnectingPrimary : Mode::ConnectingBackup;
  attemptStartedMs = millis();
  WiFi.mode(apActive ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(profiles[slot].ssid.c_str(), profiles[slot].password.c_str());
  Serial.printf("{\"type\":\"WIFI_STATUS\",\"mode\":\"CONNECTING\",\"slot\":%u,\"ssid\":\"%s\"}\n",
                slot + 1, profiles[slot].ssid.c_str());
}
}

void begin() {
  const uint32_t suffix = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFF);
  char name[32];
  snprintf(name, sizeof(name), "KNX-Analyzer-%04X", suffix);
  apSsid = name;
  prefs.begin("knxwifi", false);
  profiles[0].ssid = prefs.getString("ssid1", "");
  profiles[0].password = prefs.getString("pass1", "");
  profiles[1].ssid = prefs.getString("ssid2", "");
  profiles[1].password = prefs.getString("pass2", "");
  WiFi.setAutoReconnect(false);
  if (profiles[0].ssid.isEmpty() && profiles[1].ssid.isEmpty()) startAP();
  else tryProfile(0);
}

void tick() {
  if (dnsActive) dns.processNextRequest();
  if (connectPending) {
    connectPending = false;
    WiFi.disconnect(false, false);
    tryProfile(0);
  }
  if (current == Mode::ConnectingPrimary || current == Mode::ConnectingBackup) {
    if (WiFi.status() == WL_CONNECTED) {
      current = Mode::Station;
      disconnectedSinceMs = 0;
      if (apActive) {
        dns.stop();
        dnsActive = false;
        WiFi.softAPdisconnect(true);
        apActive = false;
        WiFi.mode(WIFI_STA);
      }
      Serial.printf("{\"type\":\"WIFI_STATUS\",\"mode\":\"STA\",\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}\n",
                    WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else if (millis() - attemptStartedMs >= kConnectTimeoutMs) {
      if (current == Mode::ConnectingPrimary) tryProfile(1);
      else startAP();
    }
  } else if (current == Mode::Station) {
    if (WiFi.status() == WL_CONNECTED) disconnectedSinceMs = 0;
    else if (disconnectedSinceMs == 0) disconnectedSinceMs = millis();
    else if (millis() - disconnectedSinceMs >= 10000) {
      disconnectedSinceMs = 0;
      tryProfile(0);
    }
  }
}

Status status() {
  String ssid;
  String ip;
  int rssi = 0;
  if (current == Mode::Station) {
    ssid = WiFi.SSID();
    ip = WiFi.localIP().toString();
    rssi = WiFi.RSSI();
  } else if (apActive) {
    ssid = apSsid;
    ip = WiFi.softAPIP().toString();
  } else {
    ssid = current == Mode::ConnectingPrimary ? profiles[0].ssid : profiles[1].ssid;
  }
  return {current, ssid, ip, apSsid, rssi, !profiles[0].ssid.isEmpty(), !profiles[1].ssid.isEmpty()};
}

bool saveProfile(uint8_t slot, const String &ssid, const String &password) {
  if (slot > 1 || ssid.length() > 32 || password.length() > 63) return false;
  const char *ssidKey = slot == 0 ? "ssid1" : "ssid2";
  const char *passwordKey = slot == 0 ? "pass1" : "pass2";
  if (prefs.putString(ssidKey, ssid) == 0 && !ssid.isEmpty()) return false;
  if (prefs.putString(passwordKey, password) == 0 && !password.isEmpty()) return false;
  profiles[slot] = {ssid, password};
  connectPending = true;
  Serial.printf("{\"type\":\"WIFI_CONFIG\",\"slot\":%u,\"saved\":true}\n", slot + 1);
  return true;
}

void reconnect() { connectPending = true; }
void forceAP() { startAP(); }
int scanComplete() { return WiFi.scanComplete(); }
void startScan() {
  if (WiFi.scanComplete() == -1) return;
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
}

const char *modeName(Mode mode) {
  switch (mode) {
    case Mode::ConnectingPrimary: return "CONNECTING_PRIMARY";
    case Mode::ConnectingBackup: return "CONNECTING_BACKUP";
    case Mode::Station: return "STA";
    default: return "AP";
  }
}
}
