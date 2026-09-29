#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <esp_crc.h>
#include <atomic>

namespace autocal {

// Experimental V1 field parameters. They are deliberately centralized and are
// not claimed to be universal KNX constants.
struct Config {
  static constexpr uint32_t WINDOW_SAMPLES = 83333;
  static constexpr uint16_t QUIET_WINDOWS_REQUIRED = 30;
  static constexpr uint16_t INDEPENDENT_BURSTS_REQUIRED = 20;
  static constexpr uint16_t ACTIVE_WINDOWS_REQUIRED = 3;
  static constexpr uint8_t SEPARATION_RATIO_REQUIRED = 4;
  static constexpr uint8_t STABLE_ESTIMATES_REQUIRED = 3;
  static constexpr uint8_t STABILITY_PERCENT = 10;
  static constexpr uint8_t CONFIDENCE_REQUIRED = 80;
  static constexpr uint16_t BOOTSTRAP_QUIET_P999_MAX = 255;
  static constexpr uint16_t BOOTSTRAP_QUIET_MAX = 512;
  static constexpr uint16_t BURST_RELEASE_SAMPLES = 833; // approximately 10 ms
  static constexpr uint16_t D44_BIN_WIDTH = 64;
  static constexpr uint8_t D44_BINS = 65; // last bin includes >=4096
};

enum class State : uint8_t {
  UNCALIBRATED, OBSERVING_NOISE, WAITING_FOR_ACTIVITY, CALIBRATING, CALIBRATED, CALIBRATION_STALE
};

inline const char *stateName(State state) {
  switch (state) {
    case State::UNCALIBRATED: return "UNCALIBRATED";
    case State::OBSERVING_NOISE: return "OBSERVING_NOISE";
    case State::WAITING_FOR_ACTIVITY: return "WAITING_FOR_ACTIVITY";
    case State::CALIBRATING: return "CALIBRATING";
    case State::CALIBRATED: return "CALIBRATED";
    default: return "CALIBRATION_STALE";
  }
}

struct Persisted {
  uint32_t magic = 0x314C4143; // CAL1
  uint16_t schema = 1;
  uint16_t bytes = sizeof(Persisted);
  uint32_t hardwareProfile = 0x53330101; // XIAO S3 / GPIO1 / ADC1 CH0 / 12 dB
  uint32_t algorithmVersion = 1;
  uint32_t sampleRate = 83333;
  uint32_t threshold = 0;
  uint32_t noiseUpper = 0;
  uint32_t activityP10 = 0;
  uint32_t quietWindows = 0;
  uint32_t activeWindows = 0;
  uint32_t bursts = 0;
  uint64_t observedSamples = 0;
  uint64_t adcZero = 0, adcLow16 = 0, adcHigh4079 = 0, adcMax4095 = 0;
  uint64_t calibratedUnixMs = 0;
  uint8_t confidence = 0;
  uint8_t reserved[7]{};
  uint32_t crc = 0;
};

class Calibration {
 public:
  void begin() {
    state_ = State::OBSERVING_NOISE;
    Preferences prefs;
    if (!prefs.begin("knx-cal-v1", true)) return;
    Persisted saved{};
    const size_t read = prefs.getBytes("record", &saved, sizeof(saved));
    prefs.end();
    const uint32_t crc = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&saved), offsetof(Persisted, crc));
    if (read == sizeof(saved) && saved.magic == Persisted{}.magic && saved.schema == 1 &&
        saved.bytes == sizeof(Persisted) && saved.hardwareProfile == Persisted{}.hardwareProfile &&
        saved.algorithmVersion == 1 && saved.sampleRate == 83333 && saved.threshold && saved.crc == crc) {
      persisted_ = saved;
      validatedThreshold_ = saved.threshold;
      noiseUpper_ = saved.noiseUpper;
      activityP10_ = saved.activityP10;
      quietWindows_ = saved.quietWindows;
      activeWindows_ = saved.activeWindows;
      independentBursts_ = saved.bursts;
      observedSamples_ = saved.observedSamples;
      adcZero_ = saved.adcZero; adcLow16_ = saved.adcLow16;
      adcHigh4079_ = saved.adcHigh4079; adcMax4095_ = saved.adcMax4095;
      confidence_ = saved.confidence;
      calibrationCrc_ = saved.crc;
      state_ = State::CALIBRATED;
      persistedValid_ = true;
    }
  }

  void process(uint16_t adc, uint32_t d44) {
    ++observedSamples_; ++windowSamples_;
    if (adc == 0) ++adcZero_;
    if (adc <= 16) ++adcLow16_;
    if (adc >= 4079) ++adcHigh4079_;
    if (adc == 4095) ++adcMax4095_;
    const uint8_t bin = d44 >= 4096 ? 64 : uint8_t(d44 / Config::D44_BIN_WIDTH);
    ++windowHistogram_[bin];
    if (d44 > windowMax_) windowMax_ = d44;

    if (noiseReady()) {
      const uint32_t gate = candidateGate();
      if (d44 > gate) {
        windowHadActivity_ = true;
        if (!burstActive_) { burstActive_ = true; burstPeak_ = d44; belowGateSamples_ = 0; }
        else if (d44 > burstPeak_) burstPeak_ = d44;
      } else if (burstActive_) {
        if (++belowGateSamples_ >= Config::BURST_RELEASE_SAMPLES) finishBurst();
      }
    }
    if (windowSamples_ >= Config::WINDOW_SAMPLES) finishWindow();
  }

  bool valid() const { return state_ == State::CALIBRATED && validatedThreshold_ != 0; }
  State state() const { return state_; }
  const char *reason() const {
    if (state_ == State::OBSERVING_NOISE || state_ == State::UNCALIBRATED) return "characterizing_background_noise";
    if (state_ == State::WAITING_FOR_ACTIVITY) return "insufficient_independent_activity";
    if (state_ == State::CALIBRATING) return "stabilizing_detection_threshold";
    if (state_ == State::CALIBRATION_STALE) return "persisted_calibration_incompatible";
    return "ready";
  }
  uint32_t validatedThreshold() const { return validatedThreshold_; }
  uint32_t candidateThreshold() const { return candidateThreshold_; }
  uint32_t noiseUpper() const { return noiseUpper_; }
  uint32_t activityP10() const { return activityP10_; }
  uint32_t quietWindows() const { return quietWindows_; }
  uint32_t activeWindows() const { return activeWindows_; }
  uint32_t bursts() const { return independentBursts_; }
  uint64_t observedSamples() const { return observedSamples_; }
  uint8_t confidence() const { return confidence_; }
  const char *confidenceName() const { return confidence_ >= 85 ? "HIGH" : confidence_ >= 60 ? "MEDIUM" : "LOW"; }
  uint64_t adcZero() const { return adcZero_; }
  uint64_t adcLow16() const { return adcLow16_; }
  uint64_t adcHigh4079() const { return adcHigh4079_; }
  uint64_t adcMax4095() const { return adcMax4095_; }
  uint32_t identityCrc() const { return calibrationCrc_; }
  bool persistedValid() const { return persistedValid_; }
  bool persistencePending() const { return persistPending_.load(); }

  void servicePersistence() {
    if (!persistPending_.exchange(false)) return;
    Persisted record{};
    record.threshold = validatedThreshold_; record.noiseUpper = noiseUpper_; record.activityP10 = activityP10_;
    record.quietWindows = quietWindows_; record.activeWindows = activeWindows_; record.bursts = independentBursts_;
    record.observedSamples = observedSamples_; record.adcZero = adcZero_; record.adcLow16 = adcLow16_;
    record.adcHigh4079 = adcHigh4079_; record.adcMax4095 = adcMax4095_;
    record.confidence = confidence_;
    record.crc = esp_crc32_le(0, reinterpret_cast<const uint8_t *>(&record), offsetof(Persisted, crc));
    Preferences prefs;
    if (prefs.begin("knx-cal-v1", false)) {
      persistedValid_ = prefs.putBytes("record", &record, sizeof(record)) == sizeof(record);
      prefs.end();
      if (persistedValid_) { persisted_ = record; calibrationCrc_ = record.crc; }
    }
  }

 private:
  static uint32_t percentile(const uint64_t *histogram, uint64_t total, uint32_t numerator, uint32_t denominator) {
    if (!total) return 0;
    const uint64_t target = (total * numerator + denominator - 1) / denominator;
    uint64_t cumulative = 0;
    for (uint8_t i = 0; i < Config::D44_BINS; ++i) {
      cumulative += histogram[i];
      if (cumulative >= target) return i == 64 ? 4096 : uint32_t(i + 1) * Config::D44_BIN_WIDTH - 1;
    }
    return 4096;
  }
  bool noiseReady() const { return quietWindows_ >= Config::QUIET_WINDOWS_REQUIRED && noiseUpper_ != 0; }
  uint32_t candidateGate() const {
    const uint32_t relative = noiseUpper_ * Config::SEPARATION_RATIO_REQUIRED;
    return relative > noiseUpper_ + 256 ? relative : noiseUpper_ + 256;
  }
  void finishBurst() {
    burstActive_ = false; belowGateSamples_ = 0;
    ++independentBursts_;
    const uint8_t bin = burstPeak_ >= 4096 ? 64 : uint8_t(burstPeak_ / Config::D44_BIN_WIDTH);
    ++activityPeakHistogram_[bin]; burstPeak_ = 0;
  }
  void finishWindow() {
    const uint32_t p999 = percentile(windowHistogram_, windowSamples_, 999, 1000);
    const bool quiet = p999 <= Config::BOOTSTRAP_QUIET_P999_MAX && windowMax_ <= Config::BOOTSTRAP_QUIET_MAX;
    if (quiet && state_ != State::CALIBRATED) {
      for (uint8_t i = 0; i < Config::D44_BINS; ++i) noiseHistogram_[i] += windowHistogram_[i];
      noiseSamples_ += windowSamples_; ++quietWindows_;
      const uint32_t p9999 = percentile(noiseHistogram_, noiseSamples_, 9999, 10000);
      noiseUpper_ = p9999 + max<uint32_t>(64, p9999 / 2);
    }
    if (windowHadActivity_) ++activeWindows_;

    if (state_ != State::CALIBRATED) {
      if (!noiseReady()) state_ = State::OBSERVING_NOISE;
      else if (independentBursts_ < Config::INDEPENDENT_BURSTS_REQUIRED || activeWindows_ < Config::ACTIVE_WINDOWS_REQUIRED)
        state_ = State::WAITING_FOR_ACTIVITY;
      else {
        activityP10_ = percentile(activityPeakHistogram_, independentBursts_, 1, 10);
        const bool separated = activityP10_ >= noiseUpper_ * Config::SEPARATION_RATIO_REQUIRED;
        if (separated) {
          candidateThreshold_ = noiseUpper_ + (activityP10_ - noiseUpper_) / 2;
          state_ = State::CALIBRATING;
          if (lastCandidate_) {
            const uint32_t delta = candidateThreshold_ > lastCandidate_ ? candidateThreshold_ - lastCandidate_ : lastCandidate_ - candidateThreshold_;
            if (uint64_t(delta) * 100 <= uint64_t(lastCandidate_) * Config::STABILITY_PERCENT) ++stableEstimates_;
            else stableEstimates_ = 0;
          }
          lastCandidate_ = candidateThreshold_;
        }
        confidence_ = confidenceScore(separated);
        if (separated && stableEstimates_ >= Config::STABLE_ESTIMATES_REQUIRED &&
            confidence_ >= Config::CONFIDENCE_REQUIRED && clippingAcceptable()) {
          validatedThreshold_ = candidateThreshold_;
          state_ = State::CALIBRATED;
          persistPending_ = true;
        }
      }
    }
    memset(windowHistogram_, 0, sizeof(windowHistogram_)); windowSamples_ = 0; windowMax_ = 0; windowHadActivity_ = false;
  }
  uint8_t confidenceScore(bool separated) const {
    uint32_t score = min<uint32_t>(25, quietWindows_ * 25 / Config::QUIET_WINDOWS_REQUIRED);
    score += min<uint32_t>(15, independentBursts_ * 15 / Config::INDEPENDENT_BURSTS_REQUIRED);
    score += min<uint32_t>(10, activeWindows_ * 10 / Config::ACTIVE_WINDOWS_REQUIRED);
    if (separated && noiseUpper_) score += min<uint32_t>(30, activityP10_ * 30 / (noiseUpper_ * Config::SEPARATION_RATIO_REQUIRED));
    score += min<uint32_t>(15, stableEstimates_ * 15 / Config::STABLE_ESTIMATES_REQUIRED);
    if (clippingAcceptable()) score += 5;
    return min<uint32_t>(100, score);
  }
  bool clippingAcceptable() const {
    if (!observedSamples_) return true;
    return (adcZero_ + adcMax4095_) * 10000ULL < observedSamples_; // below 0.01%
  }

  State state_ = State::UNCALIBRATED;
  uint64_t windowHistogram_[Config::D44_BINS]{}, noiseHistogram_[Config::D44_BINS]{};
  uint64_t activityPeakHistogram_[Config::D44_BINS]{};
  uint64_t observedSamples_ = 0, noiseSamples_ = 0, windowSamples_ = 0;
  uint64_t adcZero_ = 0, adcLow16_ = 0, adcHigh4079_ = 0, adcMax4095_ = 0;
  uint32_t quietWindows_ = 0, activeWindows_ = 0, independentBursts_ = 0;
  uint32_t windowMax_ = 0, noiseUpper_ = 0, activityP10_ = 0;
  uint32_t candidateThreshold_ = 0, validatedThreshold_ = 0, lastCandidate_ = 0;
  uint32_t burstPeak_ = 0; uint16_t belowGateSamples_ = 0;
  uint8_t stableEstimates_ = 0, confidence_ = 0;
  bool burstActive_ = false, windowHadActivity_ = false, persistedValid_ = false;
  uint32_t calibrationCrc_ = 0;
  Persisted persisted_{};
  std::atomic<bool> persistPending_{false};
};

} // namespace autocal
