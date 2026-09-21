#include "acquisition.h"
#include "analysis.h"

#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <hal/adc_types.h>
#include <soc/soc_caps.h>

namespace scope {
namespace {

constexpr uint32_t kFrameBytes = 1024;
constexpr uint32_t kDriverPoolBytes = 32768;
static_assert(SOC_ADC_DIGI_RESULT_BYTES == 4, "Expected ESP32-C6 ADC DMA format");
static_assert(kPreSamples + kPostSamples == kRingSamples, "Capture must fit ring");

adc_continuous_handle_t adcHandle = nullptr;
uint16_t *ring = nullptr;
TaskHandle_t readerTask = nullptr;
portMUX_TYPE statsMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t poolOverruns = 0;
volatile bool manualRequested = false;
volatile TriggerMode mode = TriggerMode::Manual;
volatile bool readerRunning = false;
volatile bool stopPending = false;

struct State {
  uint32_t samples = 0;
  uint32_t invalid = 0;
  uint32_t readErrors = 0;
  uint32_t ringHead = 0;
  uint32_t ringValid = 0;
  uint32_t ringWraps = 0;
  uint32_t captureNumber = 0;
  uint32_t triggerIndex = 0;
  uint32_t preCount = 0;
  uint32_t postCount = 0;
  uint16_t latestRaw = 0;
  uint16_t captureMin = 0;
  uint16_t captureMax = 0;
  uint32_t overrunsAtComplete = 0;
  uint32_t completeMicros = 0;
  uint32_t windowCount = 0;
  uint64_t windowSum = 0;
  uint16_t windowMin = UINT16_MAX;
  uint16_t windowMax = 0;
  bool running = false;
  bool captured = false;
};
State state;
uint32_t lastStatsMs = 0;
uint32_t lastStatsSamples = 0;
uint32_t lastMeasuredHz = 0;

bool IRAM_ATTR onPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  ++poolOverruns;
  return false;
}

void reader(void *) {
  alignas(4) uint8_t dma[kFrameBytes];
  uint16_t previous = 0;
  bool hasPrevious = false;
  while (true) {
    if (!readerRunning) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint32_t bytes = 0;
    esp_err_t result = adc_continuous_read(adcHandle, dma, sizeof(dma), &bytes, 100);
    if (result == ESP_ERR_TIMEOUT) continue;
    if (result != ESP_OK) {
      if (!readerRunning) continue;
      portENTER_CRITICAL(&statsMux);
      ++state.readErrors;
      portEXIT_CRITICAL(&statsMux);
      vTaskDelay(1);
      continue;
    }

    uint32_t blockSamples = 0;
    uint32_t blockInvalid = 0;
    uint32_t blockWraps = 0;
    uint32_t blockCount = 0;
    uint64_t blockSum = 0;
    uint16_t blockMin = UINT16_MAX;
    uint16_t blockMax = 0;
    bool completed = false;
    for (uint32_t offset = 0; offset + sizeof(adc_digi_output_data_t) <= bytes; offset += sizeof(adc_digi_output_data_t)) {
      const auto *sample = reinterpret_cast<const adc_digi_output_data_t *>(dma + offset);
      if (sample->type2.channel != ADC_CHANNEL_5) {
        ++blockInvalid;
        continue;
      }
      const uint16_t raw = sample->type2.data;
      state.latestRaw = raw;
      const uint32_t position = state.ringHead;
      ring[position] = raw;
      state.ringHead = position + 1;
      if (state.ringHead == kRingSamples) {
        state.ringHead = 0;
        ++blockWraps;
      }
      if (state.ringValid < kRingSamples) ++state.ringValid;
      ++blockSamples;
      ++blockCount;
      blockSum += raw;
      if (raw < blockMin) blockMin = raw;
      if (raw > blockMax) blockMax = raw;

      if (state.postCount != 0) {
        ++state.postCount;
        if (state.postCount == kPostSamples) {
          completed = true;
          break;
        }
      } else if (state.ringValid >= kPreSamples + 1) {
        const TriggerMode selected = mode;
        const bool crossingUp = hasPrevious && previous < kTriggerThresholdRaw && raw >= kTriggerThresholdRaw;
        const bool crossingDown = hasPrevious && previous > kTriggerThresholdRaw && raw <= kTriggerThresholdRaw;
        const bool fire = (selected == TriggerMode::Manual && manualRequested) ||
                          (selected == TriggerMode::Rising && crossingUp) ||
                          (selected == TriggerMode::Falling && crossingDown);
        if (fire) {
          manualRequested = false;
          state.triggerIndex = position;
          state.preCount = kPreSamples;
          state.postCount = 1;
        }
      }
      previous = raw;
      hasPrevious = true;
    }
    portENTER_CRITICAL(&statsMux);
    state.samples += blockSamples;
    state.invalid += blockInvalid;
    state.ringWraps += blockWraps;
    state.windowCount += blockCount;
    state.windowSum += blockSum;
    if (blockCount) {
      if (blockMin < state.windowMin) state.windowMin = blockMin;
      if (blockMax > state.windowMax) state.windowMax = blockMax;
    }
    portEXIT_CRITICAL(&statsMux);
    if (completed) {
      // The ADC driver takes a task-owned power-management lock in start().
      // Stop from the Arduino loop task that started it, never from this reader.
      readerRunning = false;
      portENTER_CRITICAL(&statsMux);
      state.overrunsAtComplete = poolOverruns;
      state.completeMicros = micros();
      state.running = false;
      portEXIT_CRITICAL(&statsMux);
      stopPending = true;
    }
  }
}

const char *modeName(TriggerMode selected) {
  switch (selected) {
    case TriggerMode::Rising: return "rising";
    case TriggerMode::Falling: return "falling";
    default: return "manual";
  }
}

} // namespace

bool begin() {
  if (SOC_ADC_SAMPLE_FREQ_THRES_HIGH < kRequestedHz) {
    Serial.println("{\"type\":\"ADC_ERROR\",\"stage\":\"sample_limit\"}");
    return false;
  }
  ring = static_cast<uint16_t *>(heap_caps_malloc(kRingSamples * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!ring) {
    Serial.println("{\"type\":\"ADC_ERROR\",\"stage\":\"ring_alloc\"}");
    return false;
  }
  adc_continuous_handle_cfg_t handleConfig = {};
  handleConfig.max_store_buf_size = kDriverPoolBytes;
  handleConfig.conv_frame_size = kFrameBytes;
  esp_err_t result = adc_continuous_new_handle(&handleConfig, &adcHandle);
  if (result != ESP_OK) {
    Serial.printf("{\"type\":\"ADC_ERROR\",\"stage\":\"new_handle\",\"code\":%d}\n", result);
    return false;
  }
  adc_digi_pattern_config_t pattern = {};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = ADC_CHANNEL_5;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t config = {};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = kRequestedHz;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  result = adc_continuous_config(adcHandle, &config);
  if (result != ESP_OK) {
    Serial.printf("{\"type\":\"ADC_ERROR\",\"stage\":\"config\",\"code\":%d}\n", result);
    return false;
  }
  adc_continuous_evt_cbs_t callbacks = {};
  callbacks.on_pool_ovf = onPoolOverflow;
  result = adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr);
  if (result != ESP_OK) {
    Serial.printf("{\"type\":\"ADC_ERROR\",\"stage\":\"callbacks\",\"code\":%d}\n", result);
    return false;
  }
  if (xTaskCreate(reader, "adc_reader", 4096, nullptr, 4, &readerTask) != pdPASS) {
    Serial.println("{\"type\":\"ADC_ERROR\",\"stage\":\"task\"}");
    return false;
  }
  state.running = false;
  lastStatsMs = millis();
  Serial.printf("{\"type\":\"ADC_INIT\",\"mode\":\"continuous_dma\",\"gpio\":5,\"requested_hz\":%lu,\"ring_samples\":%lu,\"pre_samples\":%lu,\"post_samples\":%lu,\"heap_free\":%u}\n",
                kRequestedHz, kRingSamples, kPreSamples, kPostSamples, ESP.getFreeHeap());
  return true;
}

bool start() {
  if (!adcHandle) return false;
  if (state.running) return true;
  if (state.captured) {
    state.captured = false;
    state.ringHead = 0;
    state.ringValid = 0;
    state.postCount = 0;
  }
  const esp_err_t result = adc_continuous_start(adcHandle);
  if (result != ESP_OK) {
    Serial.printf("{\"type\":\"ADC_ERROR\",\"stage\":\"start\",\"code\":%d}\n", result);
    return false;
  }
  portENTER_CRITICAL(&statsMux);
  state.running = true;
  portEXIT_CRITICAL(&statsMux);
  readerRunning = true;
  return true;
}

bool stop() {
  if (!adcHandle) return false;
  if (!state.running) return true;
  readerRunning = false;
  const esp_err_t result = adc_continuous_stop(adcHandle);
  portENTER_CRITICAL(&statsMux);
  state.running = false;
  portEXIT_CRITICAL(&statsMux);
  return result == ESP_OK;
}

bool arm(TriggerMode trigger) {
  if (!adcHandle) return false;
  mode = trigger;
  manualRequested = false;
  if (state.captured) {
    state.captured = false;
    state.ringHead = 0;
    state.ringValid = 0;
    state.postCount = 0;
  }
  return true;
}

bool manualTrigger() {
  if (!state.running || state.ringValid < kPreSamples + 1) return false;
  mode = TriggerMode::Manual;
  manualRequested = true;
  return true;
}

void clear() {
  if (state.captured) {
    state.captured = false;
    state.ringHead = 0;
    state.ringValid = 0;
    state.postCount = 0;
  }
}

Status status() {
  State snapshot;
  portENTER_CRITICAL(&statsMux);
  snapshot = state;
  portEXIT_CRITICAL(&statsMux);
  return {adcHandle != nullptr, snapshot.running, snapshot.captured, kRequestedHz, lastMeasuredHz,
          snapshot.samples, poolOverruns, snapshot.invalid, snapshot.readErrors, snapshot.ringValid,
          snapshot.ringWraps, snapshot.captureNumber, snapshot.latestRaw, snapshot.captureMin,
          snapshot.captureMax, mode};
}

bool waveform(Waveform &out) {
  if (!ring) return false;
  State snapshot;
  portENTER_CRITICAL(&statsMux);
  snapshot = state;
  portEXIT_CRITICAL(&statsMux);
  const uint32_t count = snapshot.captured ? kRingSamples : snapshot.ringValid;
  if (count == 0) return false;
  const uint32_t startIndex = snapshot.captured
    ? (snapshot.triggerIndex + kRingSamples - kPreSamples) % kRingSamples
    : (snapshot.ringHead + kRingSamples - count) % kRingSamples;
  out.captured = snapshot.captured;
  out.samples = count;
  out.triggerPosition = snapshot.captured ? kPreSamples : 0;
  out.minRaw = UINT16_MAX;
  out.maxRaw = 0;
  for (uint32_t x = 0; x < kWaveColumns; ++x) {
    uint16_t low = UINT16_MAX;
    uint16_t high = 0;
    const uint32_t first = x * count / kWaveColumns;
    const uint32_t end = (x + 1) * count / kWaveColumns;
    for (uint32_t i = first; i < end; ++i) {
      const uint16_t raw = ring[(startIndex + i) % kRingSamples];
      if (raw < low) low = raw;
      if (raw > high) high = raw;
    }
    out.low[x] = low == UINT16_MAX ? 0 : low;
    out.high[x] = high;
    if (out.low[x] < out.minRaw) out.minRaw = out.low[x];
    if (high > out.maxRaw) out.maxRaw = high;
  }
  return true;
}

void service() {
  if (!stopPending) return;
  stopPending = false;
  const esp_err_t result = adc_continuous_stop(adcHandle);
  portENTER_CRITICAL(&statsMux);
  if (result == ESP_OK) {
    state.captured = true;
    ++state.captureNumber;
  } else {
    ++state.readErrors;
  }
  State snapshot = state;
  portEXIT_CRITICAL(&statsMux);
  if (result != ESP_OK) {
    Serial.printf("{\"type\":\"ADC_ERROR\",\"stage\":\"stop\",\"code\":%d}\n", result);
    return;
  }
  const uint32_t captureStart = (snapshot.triggerIndex + kRingSamples - kPreSamples) % kRingSamples;
  const uint32_t stopDelayUs = micros() - snapshot.completeMicros;
  uint16_t captureMin = UINT16_MAX;
  uint16_t captureMax = 0;
  uint16_t preMin = UINT16_MAX;
  uint16_t preMax = 0;
  uint16_t postMin = UINT16_MAX;
  uint16_t postMax = 0;
  uint64_t preSum = 0;
  uint64_t postSum = 0;
  uint32_t crossingUp = 0;
  uint32_t crossingDown = 0;
  uint16_t prior = 0;
  for (uint32_t i = 0; i < kRingSamples; ++i) {
    const uint16_t raw = ring[(captureStart + i) % kRingSamples];
    if (raw < captureMin) captureMin = raw;
    if (raw > captureMax) captureMax = raw;
    if (i < kPreSamples) {
      preSum += raw;
      if (raw < preMin) preMin = raw;
      if (raw > preMax) preMax = raw;
    } else {
      postSum += raw;
      if (raw < postMin) postMin = raw;
      if (raw > postMax) postMax = raw;
    }
    if (i != 0) {
      if (prior < kTriggerThresholdRaw && raw >= kTriggerThresholdRaw) ++crossingUp;
      if (prior > kTriggerThresholdRaw && raw <= kTriggerThresholdRaw) ++crossingDown;
    }
    prior = raw;
  }
  portENTER_CRITICAL(&statsMux);
  state.captureMin = captureMin;
  state.captureMax = captureMax;
  portEXIT_CRITICAL(&statsMux);
  Serial.printf("{\"type\":\"CAPTURE_READY\",\"number\":%lu,\"trigger\":\"%s\",\"trigger_index\":%lu,\"capture_start\":%lu,\"pre_samples\":%lu,\"post_samples\":%lu,\"ring_samples\":%lu,\"trigger_raw\":%u,\"min_raw\":%u,\"max_raw\":%u,\"pre_min\":%u,\"pre_max\":%u,\"pre_mean\":%lu,\"post_min\":%u,\"post_max\":%u,\"post_mean\":%lu,\"crossings_up\":%lu,\"crossings_down\":%lu,\"overruns_at_complete\":%lu,\"overruns_at_stop\":%lu,\"stop_delay_us\":%lu,\"heap_free\":%u}\n",
                snapshot.captureNumber, modeName(mode), snapshot.triggerIndex, captureStart, snapshot.preCount, snapshot.postCount, kRingSamples,
                ring[snapshot.triggerIndex], captureMin, captureMax, preMin, preMax, static_cast<uint32_t>(preSum / kPreSamples),
                postMin, postMax, static_cast<uint32_t>(postSum / kPostSamples), crossingUp, crossingDown,
                snapshot.overrunsAtComplete, poolOverruns, stopDelayUs, ESP.getFreeHeap());
}

void printStats() {
  const uint32_t now = millis();
  if (now - lastStatsMs < 5000) return;
  State snapshot;
  portENTER_CRITICAL(&statsMux);
  snapshot = state;
  state.windowCount = 0;
  state.windowSum = 0;
  state.windowMin = UINT16_MAX;
  state.windowMax = 0;
  portEXIT_CRITICAL(&statsMux);
  const uint32_t elapsed = now - lastStatsMs;
  const uint32_t measured = static_cast<uint32_t>((static_cast<uint64_t>(snapshot.samples - lastStatsSamples) * 1000ULL) / elapsed);
  lastStatsMs = now;
  lastStatsSamples = snapshot.samples;
  if (snapshot.running) lastMeasuredHz = measured;
  Serial.printf("{\"type\":\"ADC_STATS\",\"requested_hz\":%lu,\"measured_hz\":%lu,\"samples\":%lu,\"overruns\":%lu,\"invalid\":%lu,\"read_errors\":%lu,\"min_raw\":%u,\"max_raw\":%u,\"mean_raw\":%lu,\"ring_valid\":%lu,\"ring_wraps\":%lu,\"heap_free\":%u,\"running\":%s}\n",
                kRequestedHz, measured, snapshot.samples, poolOverruns, snapshot.invalid, snapshot.readErrors,
                snapshot.windowCount ? snapshot.windowMin : 0, snapshot.windowCount ? snapshot.windowMax : 0,
                snapshot.windowCount ? static_cast<uint32_t>(snapshot.windowSum / snapshot.windowCount) : 0,
                snapshot.ringValid, snapshot.ringWraps, ESP.getFreeHeap(), snapshot.running ? "true" : "false");
}

void pollSerial() {
  while (Serial.available()) {
    const char command = static_cast<char>(Serial.read());
    switch (command) {
      case 'S': case 's':
        analysis::start();
        break;
      case 'X': case 'x':
        analysis::stop();
        break;
      case 'M': case 'm':
        manualTrigger();
        Serial.println("{\"type\":\"ADC_COMMAND\",\"command\":\"manual_trigger\"}");
        break;
      case 'R': case 'r':
        arm(TriggerMode::Rising);
        Serial.printf("{\"type\":\"ADC_ARM\",\"trigger\":\"rising\",\"threshold_raw\":%u,\"latest_raw\":%u,\"ring_valid\":%lu}\n", kTriggerThresholdRaw, state.latestRaw, state.ringValid);
        break;
      case 'F': case 'f':
        arm(TriggerMode::Falling);
        Serial.printf("{\"type\":\"ADC_ARM\",\"trigger\":\"falling\",\"threshold_raw\":%u,\"latest_raw\":%u,\"ring_valid\":%lu}\n", kTriggerThresholdRaw, state.latestRaw, state.ringValid);
        break;
      case 'D': case 'd': mode = TriggerMode::Manual; manualRequested = false; break;
      case 'A': case 'a': {
        bool canArm;
        portENTER_CRITICAL(&statsMux);
        canArm = state.captured && !state.running;
        portEXIT_CRITICAL(&statsMux);
        if (canArm) {
          portENTER_CRITICAL(&statsMux);
          state.captured = false;
          state.ringHead = 0;
          state.ringValid = 0;
          state.postCount = 0;
          portEXIT_CRITICAL(&statsMux);
          manualRequested = false;
          if (adc_continuous_start(adcHandle) == ESP_OK) {
            portENTER_CRITICAL(&statsMux);
            state.running = true;
            portEXIT_CRITICAL(&statsMux);
            readerRunning = true;
            Serial.println("{\"type\":\"ADC_COMMAND\",\"command\":\"rearm\",\"ok\":true}");
          }
        }
        break;
      }
      default: break;
    }
  }
}

} // namespace scope
