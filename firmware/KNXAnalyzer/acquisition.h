#pragma once

#include <Arduino.h>

namespace scope {

constexpr uint32_t kRequestedHz = 83333; // ESP32-C6 SOC_ADC_SAMPLE_FREQ_THRES_HIGH
constexpr uint32_t kRingSamples = 50000;
constexpr uint32_t kPreSamples = 35000;
constexpr uint32_t kPostSamples = 15000; // includes trigger sample
constexpr uint16_t kTriggerThresholdRaw = 2048;

enum class TriggerMode : uint8_t { Manual, Rising, Falling };

struct Status {
  bool initialized;
  bool running;
  bool captured;
  uint32_t requestedHz;
  uint32_t measuredHz;
  uint32_t samples;
  uint32_t overruns;
  uint32_t invalid;
  uint32_t readErrors;
  uint32_t ringValid;
  uint32_t ringWraps;
  uint32_t captureNumber;
  uint16_t latestRaw;
  uint16_t captureMin;
  uint16_t captureMax;
  TriggerMode triggerMode;
};

constexpr uint16_t kWaveColumns = 160;
struct Waveform {
  bool captured;
  uint32_t samples;
  uint32_t triggerPosition;
  uint16_t minRaw;
  uint16_t maxRaw;
  uint16_t low[kWaveColumns];
  uint16_t high[kWaveColumns];
};

bool begin();
bool start();
bool stop();
bool arm(TriggerMode trigger);
bool manualTrigger();
void clear();
Status status();
bool waveform(Waveform &out);
void service();
void printStats();
void pollSerial();

} // namespace scope
