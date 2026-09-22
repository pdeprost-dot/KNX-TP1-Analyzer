#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "acquisition.h"
#include "events.h"

namespace storage {
constexpr const char *kRoot = "/knx-analyzer";
constexpr const char *kSessions = "/knx-analyzer/sessions";
constexpr uint16_t kFormatVersion = 1;

#pragma pack(push, 1)
struct CaptureHeader {
  char magic[8];                 // "KNXADC1\0"
  uint16_t version;              // little endian
  uint16_t headerBytes;          // 48
  uint32_t eventId;
  uint32_t sampleRateHz;
  uint32_t sampleCount;
  uint32_t triggerIndex;
  uint32_t preSamples;
  uint32_t postSamples;
  uint16_t adcMin;
  uint16_t adcMax;
  uint32_t samplesCrc32;         // IEEE CRC-32 over little-endian sample bytes
  uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(CaptureHeader) == 48, "Capture header layout must remain stable");

struct Status {
  bool detected;
  bool mounted;
  uint8_t cardType;
  uint64_t cardBytes;
  uint64_t totalBytes;
  uint64_t freeBytes;
  uint64_t bytesWritten;
  uint32_t writeErrors;
  uint32_t capturesPersisted;
  uint32_t capturesFailed;
  uint32_t currentEventCount;
  uint32_t lastEventCount;
  uint8_t queueDepth;
  uint8_t queueHighWater;
  const char *currentSession;
  const char *lastSession;
};

bool begin();
Status status();
const char *currentSessionId();
const char *lastSessionId();
bool startSession(uint32_t httpErrors);
bool stopSession(uint32_t httpErrors);
bool queueEvent(const events::Event &event);
struct Tp1JournalStatus {
  uint32_t bytes;
  uint32_t lines;
  uint32_t committedBytes;
  uint32_t syncs;
  uint32_t maxWriteUs;
  uint32_t maxSyncUs;
  bool readbackOkay;
};
bool appendTp1Chunk(const char *data, size_t length);
bool syncTp1Journal();
bool finishTp1Journal();
Tp1JournalStatus tp1JournalStatus();
bool busy();
void tick();
void flush();
bool validSessionId(const String &id);
bool listSessions(JsonDocument &doc);
bool readSession(const String &id, JsonDocument &doc);
bool listEvents(const String &id, JsonDocument &doc);
bool readEvent(const String &id, uint32_t eventId, JsonDocument &doc);
bool hasCapture(const String &id, uint32_t eventId);
bool readWaveform(const String &id, uint32_t eventId, scope::Waveform &wave, uint32_t &sampleRateHz);
File openRaw(const String &id, uint32_t eventId);
} // namespace storage
