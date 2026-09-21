#pragma once

#include <Arduino.h>

namespace network {
enum class Mode : uint8_t { ConnectingPrimary, ConnectingBackup, AccessPoint, Station };
struct Status {
  Mode mode;
  String ssid;
  String ip;
  String apSsid;
  int rssi;
  bool primaryConfigured;
  bool backupConfigured;
};
void begin();
void tick();
Status status();
bool saveProfile(uint8_t slot, const String &ssid, const String &password);
void reconnect();
void forceAP();
int scanComplete();
void startScan();
const char *modeName(Mode mode);
}
