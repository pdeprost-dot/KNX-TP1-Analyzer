#pragma once

#include <Arduino.h>

struct DetectionConfig {
  uint32_t d44Threshold;
  uint32_t preSamples;
  uint32_t postSamples;
  bool enabled;
  const char *profile;
};

class D44ActivityDetector {
 public:
  void reset() { memset(history_, 0, sizeof(history_)); position_ = count_ = 0; }
  uint32_t push(uint16_t value) {
    if (count_ < 8) {
      history_[position_] = value; position_ = (position_ + 1) & 7; ++count_; return 0;
    }
    const uint32_t recent = uint32_t(value) + history_[(position_ + 7) & 7] +
      history_[(position_ + 6) & 7] + history_[(position_ + 5) & 7];
    const uint32_t previous = uint32_t(history_[(position_ + 4) & 7]) + history_[(position_ + 3) & 7] +
      history_[(position_ + 2) & 7] + history_[(position_ + 1) & 7];
    history_[position_] = value; position_ = (position_ + 1) & 7;
    return recent > previous ? recent - previous : previous - recent;
  }
 private:
  uint16_t history_[8]{};
  uint8_t position_ = 0, count_ = 0;
};
