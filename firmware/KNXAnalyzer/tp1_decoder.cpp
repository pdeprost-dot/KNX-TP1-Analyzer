#include "tp1_decoder.h"
#include "acquisition.h"
#include "storage.h"

#include <esp_timer.h>

namespace tp1 {
namespace {
// Experimental ADC thresholds, measured with the current passive divider.
// A TP1 zero is a short downward pulse. 9600 bit/s, LSB first, even parity.
constexpr uint16_t kPulseOnRaw = 1600;
constexpr uint16_t kPulseOffRaw = 1700;
constexpr uint32_t kBitRate = 9600;
constexpr uint8_t kMaxBytes = 64;
constexpr uint32_t kCharacterEndSamples = 96; // > 11 * 83,333 / 9600
constexpr uint32_t kFrameGapSamples = 160;    // separates the later LL ACK

struct Record {
  uint64_t monotonicUs;
  uint8_t length;
  uint8_t bytes[kMaxBytes];
  uint8_t parityErrors;
  uint8_t timingErrors;
  bool overflow;
};
QueueHandle_t records = nullptr;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Status counters = {};
bool low = false;
bool characterActive = false;
uint32_t characterStart = 0;
uint16_t pulseBits = 0;
bool characterTimingError = false;
uint32_t previousCharacterStart = 0;
Record current = {};
uint32_t lastStatsMs = 0;
char journal[16384];
size_t journalLength = 0;
volatile uint32_t lastPulseMs = 0;
uint32_t lastJournalWriteMs = 0;
uint32_t lastJournalSyncMs = 0;

void increment(uint32_t Status::*member) {
  portENTER_CRITICAL(&mux);
  ++(counters.*member);
  portEXIT_CRITICAL(&mux);
}

void queueRecord() {
  if (!current.length && !current.timingErrors) return;
  if (!records || xQueueSend(records, &current, 0) != pdTRUE) increment(&Status::queueDrops);
  current = {};
}

void finishCharacter() {
  if (!characterActive) return;
  characterActive = false;
  uint8_t value = 0;
  for (uint8_t bit = 0; bit < 8; ++bit)
    if (!(pulseBits & (1u << (bit + 1)))) value |= 1u << bit;
  const bool parityOne = !(pulseBits & (1u << 9));
  const bool parityOkay = ((__builtin_popcount(value) + parityOne) & 1) == 0;
  if (!parityOkay) {
    ++current.parityErrors;
    increment(&Status::parityErrors);
  }
  if (characterTimingError || (pulseBits & (1u << 10))) {
    ++current.timingErrors;
    increment(&Status::timingErrors);
  }
  if (current.length < kMaxBytes) current.bytes[current.length++] = value;
  else current.overflow = true;
  increment(&Status::characters);
}

void startCharacter(uint32_t sampleIndex) {
  if (current.length && sampleIndex - previousCharacterStart > kFrameGapSamples) queueRecord();
  if (!current.length) current.monotonicUs = esp_timer_get_time();
  previousCharacterStart = sampleIndex;
  characterStart = sampleIndex;
  characterActive = true;
  characterTimingError = false;
  pulseBits = 1; // start bit is dominant zero
}

const char *classification(const Record &record) {
  if (record.parityErrors) return "INVALID_PARITY";
  if (record.timingErrors || record.overflow) return "INVALID_TIMING";
  if (record.length == 1) return (record.bytes[0] == 0xCC || record.bytes[0] == 0x0C || record.bytes[0] == 0xC0)
      ? "VALID_KNOWN" : "ANALOG_UNDECODED";
  if (record.length < 8) return "INCOMPLETE";
  uint8_t xorValue = 0;
  for (uint8_t i = 0; i < record.length; ++i) xorValue ^= record.bytes[i];
  if (xorValue != 0xFF) return "INVALID_CHECKSUM";
  if ((record.bytes[0] & 0x80) && record.length != 8 + (record.bytes[5] & 0x0F)) return "INCOMPLETE";
  return "VALID_UNKNOWN";
}

void emit(const Record &record) {
  char hex[kMaxBytes * 2 + 1];
  static constexpr char digits[] = "0123456789ABCDEF";
  for (uint8_t i = 0; i < record.length; ++i) {
    hex[i * 2] = digits[record.bytes[i] >> 4];
    hex[i * 2 + 1] = digits[record.bytes[i] & 15];
  }
  hex[record.length * 2] = 0;
  const char *kind = classification(record);
  char source[16] = {};
  char destination[16] = {};
  const char *addressType = nullptr;
  if (!strcmp(kind, "VALID_UNKNOWN") && (record.bytes[0] & 0x80)) {
    const uint16_t src = (static_cast<uint16_t>(record.bytes[1]) << 8) | record.bytes[2];
    const uint16_t dst = (static_cast<uint16_t>(record.bytes[3]) << 8) | record.bytes[4];
    snprintf(source, sizeof(source), "%u.%u.%u", (src >> 12) & 15, (src >> 8) & 15, src & 255);
    if (record.bytes[5] & 0x80) {
      addressType = "group";
      snprintf(destination, sizeof(destination), "%u/%u/%u", (dst >> 11) & 31, (dst >> 8) & 7, dst & 255);
    } else {
      addressType = "individual";
      snprintf(destination, sizeof(destination), "%u.%u.%u", (dst >> 12) & 15, (dst >> 8) & 15, dst & 255);
    }
  }
  portENTER_CRITICAL(&mux);
  ++counters.frames;
  if (!strcmp(kind, "VALID_KNOWN")) ++counters.validKnown;
  if (record.length == 1 && record.bytes[0] == 0xCC) ++counters.acks;
  else if (!strcmp(kind, "VALID_UNKNOWN")) ++counters.validUnknown;
  else if (!strcmp(kind, "INVALID_CHECKSUM")) ++counters.invalidChecksum;
  else if (!strcmp(kind, "INCOMPLETE")) ++counters.incomplete;
  else if (!strcmp(kind, "ANALOG_UNDECODED")) ++counters.undecoded;
  if (addressType) {
    counters.lastLength = record.length;
    strlcpy(counters.lastHex, hex, sizeof(counters.lastHex));
    snprintf(counters.lastRoute, sizeof(counters.lastRoute), "%s > %s", source, destination);
  }
  if (record.length == 1 && record.bytes[0] == 0xCC) strlcpy(counters.lastAck, "ACK", sizeof(counters.lastAck));
  portEXIT_CRITICAL(&mux);
  char extra[128] = {};
  if (addressType) snprintf(extra, sizeof(extra),
      ",\"source\":\"%s\",\"destination\":\"%s\",\"destination_type\":\"%s\",\"hop_count\":%u,\"tp_length\":%u",
      source, destination, addressType, (record.bytes[5] >> 4) & 7, record.bytes[5] & 15);
  char line[512];
  const int count = snprintf(line, sizeof(line),
      "{\"type\":\"TP1_CANDIDATE\",\"monotonic_us\":%llu,\"date_time\":null,\"classification\":\"%s\",\"raw_hex\":\"%s\",\"bytes\":%u,\"parity_errors\":%u,\"timing_errors\":%u,\"overflow\":%s%s}",
      static_cast<unsigned long long>(record.monotonicUs), kind, hex, record.length,
      record.parityErrors, record.timingErrors, record.overflow ? "true" : "false", extra);
  if (count <= 0 || count >= static_cast<int>(sizeof(line))) return;
  Serial.println(line);
  if (journalLength + count + 1 <= sizeof(journal)) {
    memcpy(journal + journalLength, line, count);
    journalLength += count;
    journal[journalLength++] = '\n';
  } else increment(&Status::journalDrops);
}

bool drainJournal(bool force) {
  if (!journalLength) return true;
  const uint32_t now = millis();
  const bool quiet = now - lastPulseMs >= 200;
  if (!force && !quiet && journalLength < 8192) return true;
  if (!force && journalLength < 512 && now - lastJournalWriteMs < 1000) return true;
  const size_t length = min<size_t>(512, journalLength);
  if (!storage::appendTp1Chunk(journal, length)) return false;
  journalLength -= length;
  if (journalLength) memmove(journal, journal + length, journalLength);
  lastJournalWriteMs = now;
  return true;
}

void syncJournal() {
  const uint32_t now = millis();
  const auto saved = storage::tp1JournalStatus();
  if (saved.bytes == saved.committedBytes) return;
  const bool quiet = now - lastPulseMs >= 500;
  if ((quiet && now - lastJournalSyncMs >= 2000) || now - lastJournalSyncMs >= 10000) {
    if (storage::syncTp1Journal()) lastJournalSyncMs = now;
  }
}
}

bool begin() {
  records = xQueueCreate(16, sizeof(Record));
  Serial.printf("{\"type\":\"TP1_INIT\",\"ready\":%s,\"pulse_on_raw\":%u,\"pulse_off_raw\":%u,\"bit_rate\":%lu}\n",
      records ? "true" : "false", kPulseOnRaw, kPulseOffRaw, kBitRate);
  return records != nullptr;
}

void reset() {
  low = false;
  characterActive = false;
  current = {};
  previousCharacterStart = 0;
  lastPulseMs = millis();
  lastJournalWriteMs = lastJournalSyncMs = millis();
  portENTER_CRITICAL(&mux);
  counters = {};
  portEXIT_CRITICAL(&mux);
}

void feed(uint16_t raw, uint32_t sampleIndex) {
  if (!records) return;
  if (characterActive && sampleIndex - characterStart > kCharacterEndSamples) finishCharacter();
  if (current.length && !characterActive && sampleIndex - previousCharacterStart > kFrameGapSamples) queueRecord();
  if (raw >= kPulseOffRaw) low = false;
  if (raw >= kPulseOnRaw || low) return;
  low = true;
  lastPulseMs = millis();
  increment(&Status::pulses);
  if (!characterActive) { startCharacter(sampleIndex); return; }
  const uint32_t delta = sampleIndex - characterStart;
  const uint32_t bit = (delta * kBitRate + scope::kRequestedHz / 2) / scope::kRequestedHz;
  const int32_t error = static_cast<int32_t>(delta * kBitRate) - static_cast<int32_t>(bit * scope::kRequestedHz);
  if (bit > 10 || abs(error) > static_cast<int32_t>(3 * scope::kRequestedHz)) characterTimingError = true;
  else pulseBits |= 1u << bit;
}

void poll() {
  if (!records) return;
  Record record;
  uint8_t budget = 4;
  while (budget-- && xQueueReceive(records, &record, 0) == pdTRUE) emit(record);
  drainJournal(false);
  syncJournal();
  const uint32_t now = millis();
  if (now - lastStatsMs >= 5000) {
    lastStatsMs = now;
    const Status s = status();
    const auto adc = scope::status();
    const auto sd = storage::status();
    const auto journalState = storage::tp1JournalStatus();
    Serial.printf("{\"type\":\"TP1_STATS\",\"pulses\":%lu,\"characters\":%lu,\"parity_errors\":%lu,\"timing_errors\":%lu,\"frames\":%lu,\"valid_frames\":%lu,\"acks\":%lu,\"valid_known\":%lu,\"valid_unknown\":%lu,\"invalid_checksum\":%lu,\"incomplete\":%lu,\"undecoded\":%lu,\"queue_drops\":%lu,\"journal_drops\":%lu,\"journal_backlog_bytes\":%u,\"journal_written_bytes\":%lu,\"journal_committed_bytes\":%lu,\"journal_syncs\":%lu,\"adc_overruns\":%lu,\"dma_errors\":%lu,\"sd_errors\":%lu,\"heap_free\":%u}\n",
        s.pulses, s.characters, s.parityErrors, s.timingErrors, s.frames,
        s.validUnknown, s.acks, s.validKnown, s.validUnknown, s.invalidChecksum,
        s.incomplete, s.undecoded, s.queueDrops, s.journalDrops,
        static_cast<unsigned>(journalLength), journalState.bytes, journalState.committedBytes,
        journalState.syncs, adc.overruns, adc.readErrors, sd.writeErrors, ESP.getFreeHeap());
  }
}

bool flush() {
  if (!records) return false;
  Record record;
  while (xQueueReceive(records, &record, 0) == pdTRUE) emit(record);
  bool okay = true;
  while (journalLength && okay) okay = drainJournal(true);
  if (okay) okay = storage::finishTp1Journal();
  const auto journalState = storage::tp1JournalStatus();
  Serial.printf("{\"type\":\"TP1_JOURNAL\",\"bytes\":%lu,\"lines\":%lu,\"saved\":%s,\"backlog_bytes\":%u,\"queue_drops\":%lu,\"journal_drops\":%lu}\n",
      journalState.bytes, journalState.lines, okay ? "true" : "false",
      static_cast<unsigned>(journalLength), counters.queueDrops, counters.journalDrops);
  return okay && !journalLength && !counters.queueDrops && !counters.journalDrops;
}

Status status() {
  portENTER_CRITICAL(&mux);
  const Status copy = counters;
  portEXIT_CRITICAL(&mux);
  return copy;
}
}
