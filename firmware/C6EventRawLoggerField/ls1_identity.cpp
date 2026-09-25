#include "ls1_identity.h"

#include <Preferences.h>
#include <SD.h>
#include <esp_mac.h>
#include <esp_system.h>

namespace {
constexpr char kNamespace[] = "knx-ls1";
constexpr char kAnalyzerKey[] = "analyzer_id";
char analyzerId[37] = {};
char sessionId[37] = {};

void makeUuid(char out[37]) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = uint8_t((bytes[6] & 0x0f) | 0x40);
  bytes[8] = uint8_t((bytes[8] & 0x3f) | 0x80);
  snprintf(out, 37,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}

bool validUuid(const String &value) {
  if (value.length() != 36) return false;
  for (uint8_t i = 0; i < 36; ++i) {
    const char c = value[i];
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (c != '-') return false;
    } else if (!isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return true;
}
}

bool ls1IdentityBegin() {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  String stored = prefs.getString(kAnalyzerKey, "");
  bool created = false;
  if (!validUuid(stored)) {
    makeUuid(analyzerId);
    created = prefs.putString(kAnalyzerKey, analyzerId) == 36;
  } else {
    stored.toCharArray(analyzerId, sizeof(analyzerId));
  }
  prefs.end();
  Serial.printf("{\"type\":\"ANALYZER_ID\",\"analyzer_id\":\"%s\",\"created\":%s}\n",
                analyzerId, created ? "true" : "false");
  return analyzerId[0] != '\0';
}

void ls1PrepareSession() {
  makeUuid(sessionId);
  Serial.printf("{\"type\":\"SESSION_ID\",\"session_id\":\"%s\"}\n", sessionId);
}

bool ls1WriteSessionStart(const String &directory, uint32_t sampleRate, uint32_t d44Threshold,
                          uint32_t preSamples, uint32_t postSamples,
                          uint32_t chunkSamples, uint32_t chunkCount, uint64_t segmentMaxBytes) {
  if (!analyzerId[0] || !sessionId[0]) return false;
  File file = SD.open(directory + "/session-start.json", FILE_WRITE);
  if (!file) return false;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BASE);
  char baseMac[18];
  snprintf(baseMac, sizeof(baseMac), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  file.printf(
      "{\"record_type\":\"SESSION_START\",\"schema_version\":\"knx-long-session-1.0\","
      "\"analyzer_id\":\"%s\",\"session_id\":\"%s\","
      "\"firmware_version\":\"field-ls3\",\"firmware_baseline\":\"f040038\","
      "\"hardware\":{\"board\":\"ESP32-C6\",\"base_mac\":\"%s\",\"adc_channel\":5,\"sample_format\":\"uint16_le\"},"
      "\"acquisition_profile\":{\"profile_id\":\"event-raw-v2\","
      "\"nominal_sample_rate_hz\":{\"numerator\":\"%u\",\"denominator\":\"1\"},"
      "\"sample_bytes\":2,\"d44_threshold\":%u,\"pre_samples\":\"%u\",\"post_samples\":\"%u\","
      "\"chunk_samples\":%u,\"chunk_pool_count\":%u},"
      "\"raw_layout\":{\"format\":\"segmented-v1\",\"segment_filename\":\"raw-%%04u.bin\",\"segment_max_bytes\":\"%llu\",\"segments_manifest\":\"segments.jsonl\",\"chunks_manifest\":\"chunks.jsonl\",\"sparse_index\":\"chunk-index.jsonl\"},"
      "\"timeline\":{\"primary\":\"sample_index_uint64\",\"sample_origin\":\"ADC_FIRST_SAMPLE\","
      "\"timestamp_origin\":\"SESSION_MONOTONIC_START\","
      "\"wall_clock\":{\"available\":false,\"utc_ns\":null,\"source\":\"NONE\",\"uncertainty_us\":null}}}\n",
      analyzerId, sessionId, baseMac, sampleRate, d44Threshold, preSamples, postSamples,
      chunkSamples, chunkCount, segmentMaxBytes);
  file.flush();
  const bool ok = file.size() > 0;
  file.close();
  Serial.printf("{\"type\":\"SESSION_START_FILE\",\"ok\":%s,\"path\":\"%s/session-start.json\"}\n",
                ok ? "true" : "false", directory.c_str());
  return ok;
}

const char *ls1AnalyzerId() { return analyzerId; }
const char *ls1SessionId() { return sessionId; }
